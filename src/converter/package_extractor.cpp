#include "converter/package_extractor.h"
#include <miniz.h>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cstring>
#include <cstdint>

namespace fs = std::filesystem;

namespace u2g {

// ---------------------------------------------------------------------------
// Gzip decompression
// ---------------------------------------------------------------------------

// Skip gzip header and return offset to the raw deflate stream.
// Returns 0 on failure.
static size_t skipGzipHeader(const uint8_t* data, size_t size) {
    if (size < 10) return 0;
    if (data[0] != 0x1f || data[1] != 0x8b) return 0; // magic
    if (data[2] != 8) return 0; // compression method must be deflate

    uint8_t flags = data[3];
    size_t pos = 10; // skip fixed 10-byte header

    // FEXTRA
    if (flags & 0x04) {
        if (pos + 2 > size) return 0;
        uint16_t extraLen = static_cast<uint16_t>(data[pos]) |
                            (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2 + extraLen;
        if (pos > size) return 0;
    }

    // FNAME — null-terminated original filename
    if (flags & 0x08) {
        while (pos < size && data[pos] != 0) ++pos;
        if (pos >= size) return 0;
        ++pos; // skip null terminator
    }

    // FCOMMENT — null-terminated comment
    if (flags & 0x10) {
        while (pos < size && data[pos] != 0) ++pos;
        if (pos >= size) return 0;
        ++pos; // skip null terminator
    }

    // FHCRC — 2-byte header CRC
    if (flags & 0x02) {
        pos += 2;
        if (pos > size) return 0;
    }

    return pos;
}

// Decompress a gzip buffer using streaming inflate.
// Returns true on success, fills `out` with decompressed data.
static bool decompressGzip(const uint8_t* data, size_t size,
                           std::vector<uint8_t>& out, Log& log) {
    size_t headerEnd = skipGzipHeader(data, size);
    if (headerEnd == 0) {
        log.error("Invalid or unsupported gzip header");
        return false;
    }

    // The deflate stream runs from headerEnd to (size - 8).
    // The last 8 bytes are CRC32 + original size (mod 2^32).
    if (size < headerEnd + 8) {
        log.error("Gzip data too short");
        return false;
    }

    const uint8_t* deflateData = data + headerEnd;
    size_t deflateSize = size - headerEnd - 8;

    mz_stream stream{};
    stream.next_in = deflateData;
    stream.avail_in = static_cast<unsigned int>(
        (deflateSize > UINT32_MAX) ? UINT32_MAX : deflateSize);

    int ret = mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS);
    if (ret != MZ_OK) {
        log.error("mz_inflateInit2 failed: " + std::string(mz_error(ret)));
        return false;
    }

    // Streaming inflate with growing buffer
    const size_t chunkSize = 1024 * 1024; // 1 MB chunks
    out.clear();
    out.resize(chunkSize);
    stream.next_out = out.data();
    stream.avail_out = static_cast<unsigned int>(chunkSize);

    size_t totalDeflateConsumed = 0;

    while (true) {
        ret = mz_inflate(&stream, MZ_NO_FLUSH);

        if (ret == MZ_STREAM_END) {
            // Done — trim output to actual size
            out.resize(stream.total_out);
            break;
        }

        if (ret != MZ_OK && ret != MZ_BUF_ERROR) {
            mz_inflateEnd(&stream);
            log.error("mz_inflate failed: " + std::string(mz_error(ret)));
            return false;
        }

        // If output buffer is full, grow it
        if (stream.avail_out == 0) {
            size_t oldSize = out.size();
            out.resize(oldSize + chunkSize);
            stream.next_out = out.data() + oldSize;
            stream.avail_out = static_cast<unsigned int>(chunkSize);
        }

        // Feed more input if we've consumed the current chunk and there's more
        if (stream.avail_in == 0) {
            totalDeflateConsumed = static_cast<size_t>(stream.total_in);
            size_t remaining = deflateSize - totalDeflateConsumed;
            if (remaining == 0) {
                // No more input but didn't get STREAM_END — try finishing
                ret = mz_inflate(&stream, MZ_FINISH);
                if (ret == MZ_STREAM_END) {
                    out.resize(stream.total_out);
                    break;
                }
                mz_inflateEnd(&stream);
                log.error("Unexpected end of deflate stream");
                return false;
            }
            stream.next_in = deflateData + totalDeflateConsumed;
            stream.avail_in = static_cast<unsigned int>(
                (remaining > UINT32_MAX) ? UINT32_MAX : remaining);
        }
    }

    mz_inflateEnd(&stream);
    return true;
}

// ---------------------------------------------------------------------------
// Tar parser
// ---------------------------------------------------------------------------

// Parse an octal ASCII string (null/space terminated) into uint64_t.
static uint64_t parseOctal(const char* str, size_t len) {
    uint64_t val = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = str[i];
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7') break;
        val = (val << 3) | static_cast<uint64_t>(c - '0');
    }
    return val;
}

// Check if a 512-byte block is all zeros (end-of-archive marker).
static bool isZeroBlock(const uint8_t* block) {
    for (int i = 0; i < 512; ++i) {
        if (block[i] != 0) return false;
    }
    return true;
}

// Validate that a path doesn't escape the target directory.
// Returns false if the path contains ".." components that could escape.
static bool isPathSafe(const std::string& path) {
    // Reject absolute paths
    if (!path.empty() && (path[0] == '/' || path[0] == '\\')) return false;

    // Check for ".." traversal
    std::string normalized = normalizePath(path);
    // Split on '/' and check for ".."
    size_t start = 0;
    int depth = 0;
    while (start <= normalized.size()) {
        size_t end = normalized.find('/', start);
        if (end == std::string::npos) end = normalized.size();
        std::string component = normalized.substr(start, end - start);
        if (component == "..") {
            --depth;
            if (depth < 0) return false; // escaping root
        } else if (!component.empty() && component != ".") {
            ++depth;
        }
        start = end + 1;
    }
    return true;
}

// Create parent directories for a file path.
static bool ensureParentDirs(const std::string& filePath, Log& log) {
    fs::path parent = fs::path(filePath).parent_path();
    if (parent.empty()) return true;
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        log.error("Failed to create directory " + parent.string() + ": " + ec.message());
        return false;
    }
    return true;
}

// Extract tar data to tempDir.
// Returns true on success.
static bool extractTar(const uint8_t* tarData, size_t tarSize,
                       const std::string& tempDir, Log& log,
                       std::atomic<bool>& cancelled) {
    size_t pos = 0;
    std::string longName; // GNU long name from 'L' entry
    int fileCount = 0;

    while (pos + 512 <= tarSize) {
        if (cancelled.load(std::memory_order_relaxed)) {
            log.warn("Extraction cancelled");
            return false;
        }

        const uint8_t* header = tarData + pos;

        // Two consecutive zero blocks mark end of archive
        if (isZeroBlock(header)) {
            if (pos + 1024 <= tarSize && isZeroBlock(header + 512)) {
                break; // end of archive
            }
            // Single zero block — skip it and continue
            pos += 512;
            continue;
        }

        // Parse header fields
        char nameField[101]{};
        std::memcpy(nameField, header, 100);
        nameField[100] = '\0';

        uint64_t fileSize = parseOctal(reinterpret_cast<const char*>(header + 124), 12);

        char typeFlag = static_cast<char>(header[156]);

        // Check for UStar prefix (bytes 345-499)
        char prefixField[156]{};
        std::memcpy(prefixField, header + 345, 155);
        prefixField[155] = '\0';

        // Build the full entry name
        std::string entryName;
        if (!longName.empty()) {
            entryName = longName;
            longName.clear();
        } else {
            std::string prefix(prefixField);
            std::string name(nameField);
            if (!prefix.empty()) {
                entryName = prefix + "/" + name;
            } else {
                entryName = name;
            }
        }

        // Normalize path separators
        for (auto& c : entryName) {
            if (c == '\\') c = '/';
        }

        // Remove leading "./" if present
        if (entryName.size() >= 2 && entryName[0] == '.' && entryName[1] == '/') {
            entryName = entryName.substr(2);
        }

        // Advance past header
        pos += 512;

        // Data size on disk is padded to 512-byte boundary
        size_t dataPadded = ((fileSize + 511) / 512) * 512;

        // Handle GNU long name extension
        if (typeFlag == 'L') {
            if (pos + fileSize > tarSize) {
                log.error("Tar truncated in GNU long name entry");
                return false;
            }
            longName.assign(reinterpret_cast<const char*>(tarData + pos), fileSize);
            // Remove trailing null bytes
            while (!longName.empty() && longName.back() == '\0') {
                longName.pop_back();
            }
            pos += dataPadded;
            continue;
        }

        // Skip directories (type '5') and other non-regular types
        if (typeFlag == '5' || typeFlag == '2' || typeFlag == '3' ||
            typeFlag == '4' || typeFlag == '6') {
            pos += dataPadded;
            continue;
        }

        // Regular file: type '0' or '\0'
        if (typeFlag != '0' && typeFlag != '\0') {
            // Unknown type — skip
            pos += dataPadded;
            continue;
        }

        // Validate we have enough data
        if (pos + fileSize > tarSize) {
            log.error("Tar truncated: entry '" + entryName + "' extends past end of data");
            return false;
        }

        // Skip empty-named entries
        if (entryName.empty()) {
            pos += dataPadded;
            continue;
        }

        // Security: ensure path doesn't escape tempDir
        if (!isPathSafe(entryName)) {
            log.warn("Skipping unsafe tar path: " + entryName);
            pos += dataPadded;
            continue;
        }

        // Construct output path
        std::string outPath = tempDir + "/" + entryName;

        // Create parent directories
        if (!ensureParentDirs(outPath, log)) {
            pos += dataPadded;
            continue;
        }

        // Write file
        std::ofstream outFile(outPath, std::ios::binary);
        if (!outFile) {
            log.error("Failed to create file: " + outPath);
            pos += dataPadded;
            continue;
        }

        if (fileSize > 0) {
            outFile.write(reinterpret_cast<const char*>(tarData + pos), fileSize);
        }
        outFile.close();

        ++fileCount;

        // Periodically log progress
        if (fileCount % 500 == 0) {
            log.info("Extracted " + std::to_string(fileCount) + " files...");
        }

        pos += dataPadded;
    }

    log.info("Extracted " + std::to_string(fileCount) + " files from archive");
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool extractPackage(const std::string& packagePath, const std::string& tempDir,
                    Log& log, std::atomic<bool>& cancelled) {
    log.info("Reading package: " + packagePath);

    // 1. Read the .unitypackage file into memory
    std::ifstream file(packagePath, std::ios::binary | std::ios::ate);
    if (!file) {
        log.error("Failed to open package file: " + packagePath);
        return false;
    }

    auto fileSize = file.tellg();
    if (fileSize <= 0) {
        log.error("Package file is empty: " + packagePath);
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> compressedData(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(compressedData.data()),
                   static_cast<std::streamsize>(fileSize))) {
        log.error("Failed to read package file: " + packagePath);
        return false;
    }
    file.close();

    log.info("Package size: " + std::to_string(compressedData.size()) + " bytes");

    if (cancelled.load(std::memory_order_relaxed)) {
        log.warn("Extraction cancelled");
        return false;
    }

    // 2. Decompress gzip
    log.info("Decompressing...");
    std::vector<uint8_t> tarData;
    if (!decompressGzip(compressedData.data(), compressedData.size(), tarData, log)) {
        return false;
    }

    log.info("Decompressed size: " + std::to_string(tarData.size()) + " bytes");

    // Free compressed data — no longer needed
    compressedData.clear();
    compressedData.shrink_to_fit();

    if (cancelled.load(std::memory_order_relaxed)) {
        log.warn("Extraction cancelled");
        return false;
    }

    // 3. Ensure temp directory exists
    {
        std::error_code ec;
        fs::create_directories(tempDir, ec);
        if (ec) {
            log.error("Failed to create temp directory: " + tempDir + ": " + ec.message());
            return false;
        }
    }

    // 4. Parse tar and extract files
    log.info("Extracting tar entries...");
    if (!extractTar(tarData.data(), tarData.size(), tempDir, log, cancelled)) {
        return false;
    }

    log.info("Package extraction complete");
    return true;
}

} // namespace u2g
