#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
#include <algorithm>
#include <cctype>

namespace u2g {

enum class AssetType {
    Texture, FBX, Material, Scene, Prefab,
    Script, Shader, Animation, Audio, Animator,
    Other
};

struct AssetEntry {
    std::string guid;
    std::string unityPath;      // e.g., "Assets/Textures/brick.png"
    std::string tempFilePath;   // path to extracted 'asset' file (may be empty if folder-only)
    std::string metaFilePath;   // path to extracted 'asset.meta' file
    AssetType type = AssetType::Other;
};

enum class LogLevel { Info, Warn, Error, Fatal };

struct ConversionProgress {
    std::string phase;
    std::string currentAsset;
    float progress = 0.0f; // 0.0 .. 1.0
};

using LogCallback   = std::function<void(LogLevel, const std::string&)>;
using ProgressCallback = std::function<void(const ConversionProgress&)>;
using GuidTable     = std::map<std::string, AssetEntry>;

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

inline std::string normalizePath(const std::string& p) {
    std::string r = p;
    for (auto& c : r) if (c == '\\') c = '/';
    // collapse duplicate slashes
    std::string out;
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i] == '/' && !out.empty() && out.back() == '/') continue;
        out += r[i];
    }
    return out;
}

inline std::string stripAssetsPrefix(const std::string& unityPath) {
    std::string p = normalizePath(unityPath);
    if (p.size() >= 7 && p.substr(0, 7) == "Assets/") return p.substr(7);
    return p;
}

inline std::string getExtension(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

inline std::string replaceExtension(const std::string& path, const std::string& newExt) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return path + newExt;
    return path.substr(0, dot) + newExt;
}

inline std::string getFilename(const std::string& path) {
    std::string p = normalizePath(path);
    auto slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

inline std::string getDirectory(const std::string& path) {
    std::string p = normalizePath(path);
    auto slash = p.rfind('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

inline std::string getStem(const std::string& path) {
    std::string fn = getFilename(path);
    auto dot = fn.rfind('.');
    return dot == std::string::npos ? fn : fn.substr(0, dot);
}

inline AssetType classifyAsset(const std::string& path) {
    std::string ext = getExtension(path);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" ||
        ext == ".bmp" || ext == ".tga" || ext == ".psd" || ext == ".exr" || ext == ".hdr")
        return AssetType::Texture;
    if (ext == ".fbx") return AssetType::FBX;
    if (ext == ".mat") return AssetType::Material;
    if (ext == ".unity") return AssetType::Scene;
    if (ext == ".prefab") return AssetType::Prefab;
    if (ext == ".cs") return AssetType::Script;
    if (ext == ".shader" || ext == ".shadergraph" || ext == ".shadersubgraph" || ext == ".cginc" || ext == ".hlsl")
        return AssetType::Shader;
    if (ext == ".anim") return AssetType::Animation;
    if (ext == ".controller" || ext == ".overridecontroller") return AssetType::Animator;
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".aif" || ext == ".aiff")
        return AssetType::Audio;
    return AssetType::Other;
}

inline std::string assetTypeName(AssetType t) {
    switch (t) {
        case AssetType::Texture:   return "Texture";
        case AssetType::FBX:       return "FBX";
        case AssetType::Material:  return "Material";
        case AssetType::Scene:     return "Scene";
        case AssetType::Prefab:    return "Prefab";
        case AssetType::Script:    return "C# Script";
        case AssetType::Shader:    return "Shader";
        case AssetType::Animation: return "Animation";
        case AssetType::Audio:     return "Audio Clip";
        case AssetType::Animator:  return "Animator Controller";
        default:                   return "Other";
    }
}

} // namespace u2g
