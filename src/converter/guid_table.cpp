#include "converter/guid_table.h"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace u2g {

static std::string readFileContents(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

GuidTable buildGuidTable(const std::string& tempDir, Log& log) {
    GuidTable table;

    if (!fs::exists(tempDir) || !fs::is_directory(tempDir)) {
        log.error("Temp directory does not exist: " + tempDir);
        return table;
    }

    for (auto& entry : fs::directory_iterator(tempDir)) {
        if (!entry.is_directory()) continue;

        std::string guid = entry.path().filename().string();
        std::string guidDir = entry.path().string();

        // Read pathname file
        std::string pathnamePath = guidDir + "/pathname";
        std::string pathnameContent = readFileContents(pathnamePath);
        std::string unityPath = trim(pathnameContent);
        if (unityPath.empty()) continue; // no pathname = skip

        // Normalize path separators
        unityPath = normalizePath(unityPath);

        // Check for asset file
        std::string assetPath = guidDir + "/asset";
        std::string metaPath  = guidDir + "/asset.meta";

        AssetEntry ae;
        ae.guid = guid;
        ae.unityPath = unityPath;
        ae.tempFilePath = fs::exists(assetPath) ? assetPath : "";
        ae.metaFilePath = fs::exists(metaPath)  ? metaPath  : "";
        ae.type = classifyAsset(unityPath);

        table[guid] = std::move(ae);
    }

    // Count by type
    int texCount = 0, fbxCount = 0, matCount = 0, sceneCount = 0, prefabCount = 0, otherCount = 0;
    for (auto& [g, e] : table) {
        switch (e.type) {
            case AssetType::Texture:  texCount++; break;
            case AssetType::FBX:      fbxCount++; break;
            case AssetType::Material: matCount++; break;
            case AssetType::Scene:    sceneCount++; break;
            case AssetType::Prefab:   prefabCount++; break;
            default: otherCount++; break;
        }
    }

    log.info("Built GUID table: " + std::to_string(table.size()) + " assets ("
             + std::to_string(texCount) + " tex, "
             + std::to_string(fbxCount) + " fbx, "
             + std::to_string(matCount) + " mat, "
             + std::to_string(sceneCount) + " scene, "
             + std::to_string(prefabCount) + " prefab, "
             + std::to_string(otherCount) + " other)");

    return table;
}

} // namespace u2g
