#include "converter/converter.h"
#include "converter/package_extractor.h"
#include "converter/guid_table.h"
#include "converter/texture_converter.h"
#include "converter/material_converter.h"
#include "converter/prefab_converter.h"
#include "converter/scene_converter.h"
#include "converter/project_writer.h"
#include "util/types.h"
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

namespace u2g {

// ---------------------------------------------------------------------------
// Helper: generate a unique temp directory name
// ---------------------------------------------------------------------------
static std::string makeTempDir() {
    auto base = fs::temp_directory_path();

    // Generate a short random suffix
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    std::ostringstream ss;
    ss << "unity2godot_" << std::hex << std::setw(8) << std::setfill('0') << dist(gen);

    fs::path dir = base / ss.str();
    fs::create_directories(dir);
    return dir.string();
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
Converter::~Converter() {
    cancel();
    if (worker_.joinable())
        worker_.join();
}

// ---------------------------------------------------------------------------
// start / cancel
// ---------------------------------------------------------------------------
void Converter::start(const std::string& packagePath, const std::string& outputDir) {
    if (running_.load())
        return;

    running_  = true;
    done_     = false;
    cancelled_ = false;

    log_.clear();
    {
        std::lock_guard<std::mutex> lk(skipMutex_);
        skipReport_.clear();
    }

    // Join any previous worker before launching a new one
    if (worker_.joinable())
        worker_.join();

    worker_ = std::thread(&Converter::run, this, packagePath, outputDir);
}

void Converter::cancel() {
    cancelled_.store(true);
}

// ---------------------------------------------------------------------------
// setProgress
// ---------------------------------------------------------------------------
void Converter::setProgress(const std::string& phase, const std::string& asset, float pct) {
    std::lock_guard<std::mutex> lk(progressMutex_);
    progress_.phase        = phase;
    progress_.currentAsset = asset;
    progress_.progress     = pct;
}

// ---------------------------------------------------------------------------
// run  — the main conversion pipeline (runs on worker thread)
// ---------------------------------------------------------------------------
void Converter::run(std::string packagePath, std::string outputDir) {
    try {
        // ---- 1. Extract package ----
        setProgress("Extracting", "", 0.0f);
        log_.info("Extracting package: " + packagePath);

        std::string tempDir = makeTempDir();
        log_.info("Temp directory: " + tempDir);

        if (!extractPackage(packagePath, tempDir, log_, cancelled_)) {
            if (cancelled_.load())
                log_.warn("Conversion cancelled during extraction.");
            else
                log_.error("Failed to extract package.");
            fs::remove_all(tempDir);
            running_ = false;
            return;
        }
        if (cancelled_.load()) { fs::remove_all(tempDir); running_ = false; return; }
        setProgress("Extracting", "", 1.0f);

        // ---- 2. Build GUID table ----
        setProgress("Building GUID table", "", 0.0f);
        log_.info("Building GUID table...");

        GuidTable guids = buildGuidTable(tempDir, log_);
        log_.info("Found " + std::to_string(guids.size()) + " assets in package.");
        if (cancelled_.load()) { fs::remove_all(tempDir); running_ = false; return; }
        setProgress("Building GUID table", "", 1.0f);

        // ---- 3. Copy textures ----
        setProgress("Copying textures", "", 0.0f);
        log_.info("Copying textures...");

        convertTextures(guids, outputDir, log_, cancelled_,
            [this](const ConversionProgress& p) {
                setProgress("Copying textures", p.currentAsset, p.progress);
            });
        if (cancelled_.load()) { fs::remove_all(tempDir); running_ = false; return; }

        // ---- 4. Copy FBX files ----
        setProgress("Copying FBX files", "", 0.0f);
        log_.info("Copying FBX files...");
        {
            // Gather FBX entries
            std::vector<const AssetEntry*> fbxEntries;
            for (auto& [guid, entry] : guids) {
                if (entry.type == AssetType::FBX && !entry.tempFilePath.empty())
                    fbxEntries.push_back(&entry);
            }

            for (size_t i = 0; i < fbxEntries.size(); ++i) {
                if (cancelled_.load()) { fs::remove_all(tempDir); running_ = false; return; }

                const AssetEntry& entry = *fbxEntries[i];
                std::string relPath = stripAssetsPrefix(entry.unityPath);
                fs::path destPath = fs::path(outputDir) / relPath;

                fs::create_directories(destPath.parent_path());

                std::error_code ec;
                fs::copy_file(entry.tempFilePath, destPath,
                              fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    log_.warn("Failed to copy FBX: " + entry.unityPath + " (" + ec.message() + ")");
                } else {
                    log_.info("Copied FBX: " + relPath);
                }

                float pct = fbxEntries.empty() ? 1.0f
                          : static_cast<float>(i + 1) / static_cast<float>(fbxEntries.size());
                setProgress("Copying FBX files", entry.unityPath, pct);
            }
        }
        if (cancelled_.load()) { fs::remove_all(tempDir); running_ = false; return; }

        // ---- 5. Convert materials ----
        setProgress("Converting materials", "", 0.0f);
        log_.info("Converting materials...");

        MaterialMap matMap = convertMaterials(guids, outputDir, log_, cancelled_,
            [this](const ConversionProgress& p) {
                setProgress("Converting materials", p.currentAsset, p.progress);
            });
        if (cancelled_.load()) { fs::remove_all(tempDir); running_ = false; return; }
        log_.info("Converted " + std::to_string(matMap.size()) + " materials.");

        // ---- 6. Convert prefabs ----
        setProgress("Converting prefabs", "", 0.0f);
        log_.info("Converting prefabs...");

        // Convert MaterialMap to std::map<string,string>
        std::map<std::string, std::string> matMapStr(matMap.begin(), matMap.end());

        PrefabMap prefabMap = convertPrefabs(guids, matMapStr, outputDir, log_, cancelled_,
            [this](const ConversionProgress& p) {
                setProgress("Converting prefabs", p.currentAsset, p.progress);
            });
        if (cancelled_.load()) { fs::remove_all(tempDir); running_ = false; return; }
        log_.info("Converted " + std::to_string(prefabMap.size()) + " prefabs.");

        // ---- 7. Convert scenes ----
        setProgress("Converting scenes", "", 0.0f);
        log_.info("Converting scenes...");

        std::map<std::string, std::string> prefabMapStr(prefabMap.begin(), prefabMap.end());

        convertScenes(guids, matMapStr, prefabMapStr, outputDir, log_, cancelled_,
            [this](const ConversionProgress& p) {
                setProgress("Converting scenes", p.currentAsset, p.progress);
            });
        if (cancelled_.load()) { fs::remove_all(tempDir); running_ = false; return; }

        // ---- 8. Write project.godot ----
        setProgress("Writing project file", "", 0.0f);
        log_.info("Writing project.godot...");

        // Derive project name from the package filename
        std::string projectName = getStem(getFilename(packagePath));
        writeProjectFile(outputDir, projectName, log_);
        setProgress("Writing project file", "", 1.0f);

        // ---- 9. Build skip report ----
        {
            // Collect skipped assets by category
            std::map<std::string, std::vector<std::string>> skippedByCategory;

            for (auto& [guid, entry] : guids) {
                // We handle: Texture, FBX, Material, Scene, Prefab
                // Everything else with a file is "skipped"
                switch (entry.type) {
                    case AssetType::Texture:
                    case AssetType::FBX:
                    case AssetType::Material:
                    case AssetType::Scene:
                    case AssetType::Prefab:
                        // These were converted/copied
                        continue;
                    default:
                        break;
                }

                // Only report entries that actually have file data
                if (entry.tempFilePath.empty())
                    continue;

                std::string catName = assetTypeName(entry.type);
                skippedByCategory[catName].push_back(entry.unityPath);
            }

            std::lock_guard<std::mutex> lk(skipMutex_);
            skipReport_.clear();
            for (auto& [cat, files] : skippedByCategory) {
                skipReport_.push_back({cat, files});
            }
        }

        // ---- 10. Cleanup temp dir ----
        log_.info("Cleaning up temp directory...");
        std::error_code ec;
        fs::remove_all(tempDir, ec);
        if (ec) {
            log_.warn("Failed to fully remove temp dir: " + ec.message());
        }

        // ---- 11. Done ----
        setProgress("Done", "", 1.0f);
        log_.info("Conversion complete!");

    } catch (const std::exception& ex) {
        log_.fatal(std::string("Unhandled exception: ") + ex.what());
        setProgress("Error", "", 0.0f);
    } catch (...) {
        log_.fatal("Unknown exception during conversion.");
        setProgress("Error", "", 0.0f);
    }

    done_    = true;
    running_ = false;
}

} // namespace u2g
