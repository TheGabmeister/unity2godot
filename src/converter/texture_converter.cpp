#include "converter/texture_converter.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace u2g {

void convertTextures(const GuidTable& guids, const std::string& outputDir,
                     Log& log, std::atomic<bool>& cancelled, ProgressCallback progress) {
    // Collect texture entries
    std::vector<const AssetEntry*> textures;
    for (auto& [guid, entry] : guids) {
        if (entry.type == AssetType::Texture && !entry.tempFilePath.empty())
            textures.push_back(&entry);
    }

    if (textures.empty()) {
        log.info("No textures to convert.");
        return;
    }

    log.info("Copying " + std::to_string(textures.size()) + " textures...");

    for (size_t i = 0; i < textures.size(); ++i) {
        if (cancelled.load()) return;

        auto& tex = *textures[i];
        std::string relPath = stripAssetsPrefix(tex.unityPath);
        std::string outPath = outputDir + "/" + relPath;

        if (progress) {
            progress({"Copying textures", getFilename(tex.unityPath),
                      (float)(i + 1) / (float)textures.size()});
        }

        // Warn about unsupported formats
        std::string ext = getExtension(tex.unityPath);
        if (ext == ".psd" || ext == ".exr") {
            log.warn("Unsupported texture format copied as-is (Godot cannot import): " + tex.unityPath);
        }

        // Create directory and copy
        fs::create_directories(fs::path(outPath).parent_path());

        std::error_code ec;
        fs::copy_file(tex.tempFilePath, outPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            log.error("Failed to copy texture " + tex.unityPath + ": " + ec.message());
        }
    }

    log.info("Textures copied: " + std::to_string(textures.size()));
}

} // namespace u2g
