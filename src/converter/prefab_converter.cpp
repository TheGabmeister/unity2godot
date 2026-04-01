#include "converter/prefab_converter.h"
#include "converter/scene_converter.h"
#include "converter/unity_yaml_parser.h"
#include "util/types.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace u2g {

PrefabMap convertPrefabs(const GuidTable& guids,
                         const std::map<std::string, std::string>& materialMap,
                         const std::string& outputDir,
                         Log& log,
                         std::atomic<bool>& cancelled,
                         ProgressCallback progress) {
    PrefabMap prefabMap;

    // Collect prefab entries from the GUID table.
    std::vector<const AssetEntry*> prefabs;
    for (auto& [guid, entry] : guids) {
        if (entry.type == AssetType::Prefab && !entry.tempFilePath.empty())
            prefabs.push_back(&entry);
    }

    if (prefabs.empty()) {
        log.info("No prefabs to convert.");
        return prefabMap;
    }

    log.info("Converting " + std::to_string(prefabs.size()) + " prefabs...");

    for (size_t i = 0; i < prefabs.size(); ++i) {
        if (cancelled.load()) return prefabMap;

        auto& entry = *prefabs[i];

        if (progress) {
            progress({"Converting prefabs", getFilename(entry.unityPath),
                      (float)(i + 1) / (float)prefabs.size()});
        }

        // Read the prefab YAML file.
        std::ifstream f(entry.tempFilePath, std::ios::binary);
        if (!f) {
            log.error("Failed to read prefab: " + entry.unityPath);
            continue;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        // Build the tscn data.
        // V1: pass an empty prefab map so prefabs don't reference other prefabs
        // (nested prefab instancing is not supported; they are flattened).
        std::map<std::string, std::string> emptyPrefabMap;
        TscnData data = buildTscnData(ss.str(), guids, materialMap, emptyPrefabMap, outputDir, log);
        if (!data.root) {
            log.error("Failed to build prefab: " + entry.unityPath);
            continue;
        }

        // Determine output path: strip Assets/ prefix, change extension to .tscn.
        std::string relPath = replaceExtension(stripAssetsPrefix(entry.unityPath), ".tscn");
        std::string outPath = outputDir + "/" + relPath;
        std::string resPath = "res://" + relPath;

        fs::create_directories(fs::path(outPath).parent_path());

        // Set root node name from the prefab filename.
        std::string prefabName = getStem(entry.unityPath);

        // If the prefab root is itself an external scene instance (eg. FBX),
        // wrap it in a plain Node3D root so nested prefab instancing in scenes
        // keeps the internal compensated transform reliably.
        if (!data.root->instanceResId.empty()) {
            auto wrapper = std::make_shared<GodotNode>();
            wrapper->name = prefabName;
            wrapper->godotType = "Node3D";

            if (data.root->name == prefabName) {
                data.root->name += "_Instance";
            }

            wrapper->children.push_back(data.root);
            data.root = std::move(wrapper);
        } else {
            data.root->name = prefabName;
        }

        writeTscnFile(outPath, data);

        prefabMap[entry.guid] = resPath;
        log.info("Converted prefab: " + entry.unityPath + " -> " + relPath);
    }

    log.info("Prefabs converted: " + std::to_string(prefabMap.size()));
    return prefabMap;
}

} // namespace u2g
