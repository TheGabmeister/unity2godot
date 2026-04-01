#include "converter/material_converter.h"
#include "converter/unity_yaml_parser.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

namespace u2g {

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Find a named entry in a YAML sequence of single-key maps.
// Unity stores m_TexEnvs, m_Floats, m_Colors as sequences like:
//   - _BaseMap:
//       m_Texture: {fileID: ..., guid: ..., type: 3}
//   - _BumpMap:
//       ...
const YamlNode& findInSeq(const YamlNode& seq, const std::string& name) {
    if (!seq.isSeq()) return YamlNode::NULL_NODE;
    for (auto& item : seq.sequence) {
        if (item.isMap()) {
            auto it = item.map.find(name);
            if (it != item.map.end()) return it->second;
        }
    }
    return YamlNode::NULL_NODE;
}

// Get a float property from m_Floats sequence.
float getFloat(const YamlNode& floats, const std::string& name, float def) {
    const YamlNode& val = findInSeq(floats, name);
    if (val.isNull()) return def;
    // m_Floats entries are like: - _Metallic: 0.5
    // so the value node is a scalar directly
    if (val.isScalar()) return val.toFloat(def);
    return def;
}

// Get a color property from m_Colors sequence.
// Returns {r, g, b, a}.
struct Color4 {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

Color4 getColor(const YamlNode& colors, const std::string& name) {
    Color4 c;
    const YamlNode& val = findInSeq(colors, name);
    if (val.isNull()) return c;
    // m_Colors entries are like: - _BaseColor: {r: 1, g: 0.8, b: 0.6, a: 1}
    if (val.isMap()) {
        c.r = val["r"].toFloat(1.0f);
        c.g = val["g"].toFloat(1.0f);
        c.b = val["b"].toFloat(1.0f);
        c.a = val["a"].toFloat(1.0f);
    }
    return c;
}

// Resolve a texture GUID to a res:// path, or return empty string.
std::string resolveTexture(const YamlNode& texEntry, const GuidTable& guids, Log& log) {
    if (texEntry.isNull()) return "";

    // texEntry is the map under the texture name, e.g.:
    //   m_Texture: {fileID: 2800000, guid: ..., type: 3}
    //   m_Scale: {x: 1, y: 1}
    //   m_Offset: {x: 0, y: 0}
    const YamlNode& texRef = texEntry["m_Texture"];
    if (texRef.isNull()) return "";

    std::string guid = texRef["guid"].str();
    if (guid.empty()) return "";

    // Null texture reference (all zeros)
    if (guid == "0000000000000000e000000000000000" ||
        guid == "0000000000000000f000000000000000") {
        return "";
    }

    int64_t fileID = texRef["fileID"].toInt(0);
    if (fileID == 0) return "";

    auto it = guids.find(guid);
    if (it == guids.end()) {
        log.warn("Texture GUID not found in package: " + guid);
        return "";
    }

    std::string relPath = stripAssetsPrefix(it->second.unityPath);
    return "res://" + relPath;
}

// Get tiling (scale) from a tex env entry.
struct Vec2 {
    float x = 1.0f, y = 1.0f;
};

Vec2 getTexScale(const YamlNode& texEntry) {
    Vec2 v;
    if (texEntry.isNull()) return v;
    const YamlNode& scale = texEntry["m_Scale"];
    if (!scale.isNull()) {
        v.x = scale["x"].toFloat(1.0f);
        v.y = scale["y"].toFloat(1.0f);
    }
    return v;
}

Vec2 getTexOffset(const YamlNode& texEntry) {
    Vec2 v{0.0f, 0.0f};
    if (texEntry.isNull()) return v;
    const YamlNode& offset = texEntry["m_Offset"];
    if (!offset.isNull()) {
        v.x = offset["x"].toFloat(0.0f);
        v.y = offset["y"].toFloat(0.0f);
    }
    return v;
}

// Format a float with reasonable precision, stripping trailing zeros.
std::string fmtFloat(float v) {
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

// Format a Godot Color literal.
std::string fmtColor(const Color4& c) {
    return "Color(" + fmtFloat(c.r) + ", " + fmtFloat(c.g) + ", "
           + fmtFloat(c.b) + ", " + fmtFloat(c.a) + ")";
}

// Format a Godot Vector3 literal.
std::string fmtVec3(float x, float y, float z) {
    return "Vector3(" + fmtFloat(x) + ", " + fmtFloat(y) + ", " + fmtFloat(z) + ")";
}

// Check if a color is effectively black (all channels near zero).
bool isBlack(const Color4& c) {
    return (c.r < 0.001f && c.g < 0.001f && c.b < 0.001f);
}

// Shader type heuristic.
enum class ShaderType {
    URPLit,
    URPUnlit,
    LegacyStandard,
    Unknown
};

ShaderType detectShader(const YamlNode& root) {
    // Check m_Shader reference for built-in Standard shader
    const YamlNode& shaderRef = root["m_Shader"];
    if (!shaderRef.isNull()) {
        int64_t fileID = shaderRef["fileID"].toInt(0);
        int64_t type = shaderRef["type"].toInt(-1);
        std::string guid = shaderRef["guid"].str();

        // Built-in Standard shader: fileID=46, type=0
        if (fileID == 46 && type == 0) {
            return ShaderType::LegacyStandard;
        }

        // Well-known URP Lit GUID
        if (guid == "933532a4fcc9baf4fa0491de14d08ed7") {
            return ShaderType::URPLit;
        }

        // Well-known URP Unlit GUID (may vary)
        if (guid == "0406db56afc94f93e804a1b66f28321") {
            return ShaderType::URPUnlit;
        }
    }

    // Heuristic: check material properties
    const YamlNode& savedProps = root["m_SavedProperties"];
    const YamlNode& texEnvs = savedProps["m_TexEnvs"];
    const YamlNode& colors = savedProps["m_Colors"];

    bool hasBaseMap = !findInSeq(texEnvs, "_BaseMap").isNull();
    bool hasBaseColor = !findInSeq(colors, "_BaseColor").isNull();
    bool hasMainTex = !findInSeq(texEnvs, "_MainTex").isNull();
    bool hasColor = !findInSeq(colors, "_Color").isNull();

    // URP uses _BaseMap / _BaseColor
    if (hasBaseMap || hasBaseColor) {
        // Distinguish Lit vs Unlit: Unlit typically lacks _Metallic, _BumpMap, etc.
        // For V1, treat all URP-style materials as Lit — Unlit detection is a refinement.
        const YamlNode& floats = savedProps["m_Floats"];
        bool hasMetallic = !findInSeq(floats, "_Metallic").isNull();
        bool hasSmoothness = !findInSeq(floats, "_Smoothness").isNull();
        bool hasBumpMap = !findInSeq(texEnvs, "_BumpMap").isNull();

        // If it has none of the Lit-specific properties, treat as Unlit
        if (!hasMetallic && !hasSmoothness && !hasBumpMap) {
            return ShaderType::URPUnlit;
        }
        return ShaderType::URPLit;
    }

    // Legacy Standard uses _MainTex / _Color
    if (hasMainTex || hasColor) {
        return ShaderType::LegacyStandard;
    }

    return ShaderType::Unknown;
}

// ---------------------------------------------------------------------------
// .tres generation
// ---------------------------------------------------------------------------

struct ExtResource {
    int id;
    std::string path; // res:// path
};

struct TresBuilder {
    std::vector<ExtResource> extResources;
    std::vector<std::pair<std::string, std::string>> properties; // key, value
    int nextExtId = 1;

    // Add an external texture resource and return its ExtResource reference string.
    std::string addTexture(const std::string& resPath) {
        int id = nextExtId++;
        extResources.push_back({id, resPath});
        return "ExtResource(\"" + std::to_string(id) + "\")";
    }

    void set(const std::string& key, const std::string& value) {
        properties.push_back({key, value});
    }

    std::string build() const {
        std::ostringstream ss;
        ss << "[gd_resource type=\"StandardMaterial3D\" format=3]\n";

        // ext_resource section
        if (!extResources.empty()) {
            ss << "\n";
            for (auto& ext : extResources) {
                ss << "[ext_resource type=\"Texture2D\" path=\""
                   << ext.path << "\" id=\"" << ext.id << "\"]\n";
            }
        }

        // resource section
        ss << "\n[resource]\n";
        for (auto& [key, val] : properties) {
            ss << key << " = " << val << "\n";
        }

        return ss.str();
    }
};

void buildURPLit(TresBuilder& tres, const YamlNode& root, const GuidTable& guids, Log& log) {
    const YamlNode& savedProps = root["m_SavedProperties"];
    const YamlNode& texEnvs = savedProps["m_TexEnvs"];
    const YamlNode& floats = savedProps["m_Floats"];
    const YamlNode& colors = savedProps["m_Colors"];

    // Albedo color
    Color4 baseColor = getColor(colors, "_BaseColor");
    tres.set("albedo_color", fmtColor(baseColor));

    // Albedo texture
    const YamlNode& baseMapEntry = findInSeq(texEnvs, "_BaseMap");
    std::string albedoTex = resolveTexture(baseMapEntry, guids, log);
    if (!albedoTex.empty()) {
        tres.set("albedo_texture", tres.addTexture(albedoTex));
    }

    // Metallic
    float metallic = getFloat(floats, "_Metallic", 0.0f);
    tres.set("metallic", fmtFloat(metallic));

    // Metallic texture
    const YamlNode& metallicMapEntry = findInSeq(texEnvs, "_MetallicGlossMap");
    std::string metallicTex = resolveTexture(metallicMapEntry, guids, log);
    if (!metallicTex.empty()) {
        tres.set("metallic_texture", tres.addTexture(metallicTex));
    }

    // Roughness (1.0 - smoothness)
    float smoothness = getFloat(floats, "_Smoothness", 0.5f);
    tres.set("roughness", fmtFloat(1.0f - smoothness));

    // Normal map
    const YamlNode& bumpMapEntry = findInSeq(texEnvs, "_BumpMap");
    std::string normalTex = resolveTexture(bumpMapEntry, guids, log);
    if (!normalTex.empty()) {
        tres.set("normal_enabled", "true");
        tres.set("normal_texture", tres.addTexture(normalTex));
        float bumpScale = getFloat(floats, "_BumpScale", 1.0f);
        if (bumpScale != 1.0f) {
            tres.set("normal_scale", fmtFloat(bumpScale));
        }
    }

    // Emission
    Color4 emissionColor = getColor(colors, "_EmissionColor");
    if (!isBlack(emissionColor)) {
        tres.set("emission_enabled", "true");
        // Emission color in Godot is RGB only (energy scales brightness)
        float maxChannel = std::max({emissionColor.r, emissionColor.g, emissionColor.b});
        Color4 normEmission = emissionColor;
        float energy = 1.0f;
        if (maxChannel > 1.0f) {
            energy = maxChannel;
            normEmission.r /= maxChannel;
            normEmission.g /= maxChannel;
            normEmission.b /= maxChannel;
        } else if (maxChannel > 0.0f) {
            energy = maxChannel;
            normEmission.r /= maxChannel;
            normEmission.g /= maxChannel;
            normEmission.b /= maxChannel;
        }
        tres.set("emission", fmtColor(normEmission));
        tres.set("emission_energy_multiplier", fmtFloat(energy));
    }

    // Emission texture
    const YamlNode& emissionMapEntry = findInSeq(texEnvs, "_EmissionMap");
    std::string emissionTex = resolveTexture(emissionMapEntry, guids, log);
    if (!emissionTex.empty()) {
        tres.set("emission_enabled", "true");
        tres.set("emission_texture", tres.addTexture(emissionTex));
    }

    // Occlusion map
    const YamlNode& aoMapEntry = findInSeq(texEnvs, "_OcclusionMap");
    std::string aoTex = resolveTexture(aoMapEntry, guids, log);
    if (!aoTex.empty()) {
        tres.set("ao_enabled", "true");
        tres.set("ao_texture", tres.addTexture(aoTex));
        float aoStrength = getFloat(floats, "_OcclusionStrength", 1.0f);
        tres.set("ao_light_affect", fmtFloat(aoStrength));
    }

    // Transparency / alpha
    float surface = getFloat(floats, "_Surface", 0.0f);
    float cutoff = getFloat(floats, "_Cutoff", -1.0f);
    if (cutoff >= 0.0f && cutoff < 1.0f) {
        // Alpha scissor mode
        tres.set("transparency", "2");
        tres.set("alpha_scissor_threshold", fmtFloat(cutoff));
    } else if (surface >= 0.5f) {
        // Transparent (alpha blend)
        tres.set("transparency", "1");
    }

    // Cull mode: 0=off, 1=front, 2=back (default)
    float cull = getFloat(floats, "_Cull", 2.0f);
    int cullInt = (int)cull;
    if (cullInt == 0) {
        tres.set("cull_mode", "0");
    } else if (cullInt == 1) {
        tres.set("cull_mode", "1");
    }
    // cullInt == 2 is default (back), omit

    // UV tiling and offset from _BaseMap
    if (!baseMapEntry.isNull()) {
        Vec2 scale = getTexScale(baseMapEntry);
        Vec2 offset = getTexOffset(baseMapEntry);

        if (scale.x != 1.0f || scale.y != 1.0f) {
            tres.set("uv1_scale", fmtVec3(scale.x, scale.y, 1.0f));
        }
        if (offset.x != 0.0f || offset.y != 0.0f) {
            tres.set("uv1_offset", fmtVec3(offset.x, offset.y, 0.0f));
        }
    }
}

void buildURPUnlit(TresBuilder& tres, const YamlNode& root, const GuidTable& guids, Log& log) {
    const YamlNode& savedProps = root["m_SavedProperties"];
    const YamlNode& texEnvs = savedProps["m_TexEnvs"];
    const YamlNode& colors = savedProps["m_Colors"];

    // Unshaded mode
    tres.set("shading_mode", "0"); // SHADING_MODE_UNSHADED

    // Albedo color
    Color4 baseColor = getColor(colors, "_BaseColor");
    tres.set("albedo_color", fmtColor(baseColor));

    // Albedo texture
    const YamlNode& baseMapEntry = findInSeq(texEnvs, "_BaseMap");
    std::string albedoTex = resolveTexture(baseMapEntry, guids, log);
    if (!albedoTex.empty()) {
        tres.set("albedo_texture", tres.addTexture(albedoTex));
    }

    // UV tiling and offset from _BaseMap
    if (!baseMapEntry.isNull()) {
        Vec2 scale = getTexScale(baseMapEntry);
        Vec2 offset = getTexOffset(baseMapEntry);

        if (scale.x != 1.0f || scale.y != 1.0f) {
            tres.set("uv1_scale", fmtVec3(scale.x, scale.y, 1.0f));
        }
        if (offset.x != 0.0f || offset.y != 0.0f) {
            tres.set("uv1_offset", fmtVec3(offset.x, offset.y, 0.0f));
        }
    }
}

void buildLegacyStandard(TresBuilder& tres, const YamlNode& root, const GuidTable& guids, Log& log) {
    const YamlNode& savedProps = root["m_SavedProperties"];
    const YamlNode& texEnvs = savedProps["m_TexEnvs"];
    const YamlNode& floats = savedProps["m_Floats"];
    const YamlNode& colors = savedProps["m_Colors"];

    // Albedo color
    Color4 baseColor = getColor(colors, "_Color");
    tres.set("albedo_color", fmtColor(baseColor));

    // Albedo texture
    const YamlNode& mainTexEntry = findInSeq(texEnvs, "_MainTex");
    std::string albedoTex = resolveTexture(mainTexEntry, guids, log);
    if (!albedoTex.empty()) {
        tres.set("albedo_texture", tres.addTexture(albedoTex));
    }

    // Metallic texture
    const YamlNode& metallicMapEntry = findInSeq(texEnvs, "_MetallicGlossMap");
    std::string metallicTex = resolveTexture(metallicMapEntry, guids, log);
    if (!metallicTex.empty()) {
        tres.set("metallic_texture", tres.addTexture(metallicTex));
    }

    // Roughness (1.0 - glossiness)
    float glossiness = getFloat(floats, "_Glossiness", 0.5f);
    tres.set("roughness", fmtFloat(1.0f - glossiness));

    // Normal map
    const YamlNode& bumpMapEntry = findInSeq(texEnvs, "_BumpMap");
    std::string normalTex = resolveTexture(bumpMapEntry, guids, log);
    if (!normalTex.empty()) {
        tres.set("normal_enabled", "true");
        tres.set("normal_texture", tres.addTexture(normalTex));
    }

    // Emission
    Color4 emissionColor = getColor(colors, "_EmissionColor");
    if (!isBlack(emissionColor)) {
        tres.set("emission_enabled", "true");
        float maxChannel = std::max({emissionColor.r, emissionColor.g, emissionColor.b});
        Color4 normEmission = emissionColor;
        float energy = 1.0f;
        if (maxChannel > 1.0f) {
            energy = maxChannel;
            normEmission.r /= maxChannel;
            normEmission.g /= maxChannel;
            normEmission.b /= maxChannel;
        } else if (maxChannel > 0.0f) {
            energy = maxChannel;
            normEmission.r /= maxChannel;
            normEmission.g /= maxChannel;
            normEmission.b /= maxChannel;
        }
        tres.set("emission", fmtColor(normEmission));
        tres.set("emission_energy_multiplier", fmtFloat(energy));
    }

    // Emission texture
    const YamlNode& emissionMapEntry = findInSeq(texEnvs, "_EmissionMap");
    std::string emissionTex = resolveTexture(emissionMapEntry, guids, log);
    if (!emissionTex.empty()) {
        tres.set("emission_enabled", "true");
        tres.set("emission_texture", tres.addTexture(emissionTex));
    }

    // Occlusion map
    const YamlNode& aoMapEntry = findInSeq(texEnvs, "_OcclusionMap");
    std::string aoTex = resolveTexture(aoMapEntry, guids, log);
    if (!aoTex.empty()) {
        tres.set("ao_enabled", "true");
        tres.set("ao_texture", tres.addTexture(aoTex));
    }

    // UV tiling and offset from _MainTex
    if (!mainTexEntry.isNull()) {
        Vec2 scale = getTexScale(mainTexEntry);
        Vec2 offset = getTexOffset(mainTexEntry);

        if (scale.x != 1.0f || scale.y != 1.0f) {
            tres.set("uv1_scale", fmtVec3(scale.x, scale.y, 1.0f));
        }
        if (offset.x != 0.0f || offset.y != 0.0f) {
            tres.set("uv1_offset", fmtVec3(offset.x, offset.y, 0.0f));
        }
    }
}

void buildDefaultMaterial(TresBuilder& tres) {
    Color4 white{1.0f, 1.0f, 1.0f, 1.0f};
    tres.set("albedo_color", fmtColor(white));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

MaterialMap convertMaterials(const GuidTable& guids, const std::string& outputDir,
                             Log& log, std::atomic<bool>& cancelled, ProgressCallback progress) {
    MaterialMap result;

    // Collect material entries
    std::vector<const AssetEntry*> materials;
    for (auto& [guid, entry] : guids) {
        if (entry.type == AssetType::Material && !entry.tempFilePath.empty())
            materials.push_back(&entry);
    }

    if (materials.empty()) {
        log.info("No materials to convert.");
        return result;
    }

    log.info("Converting " + std::to_string(materials.size()) + " materials...");

    for (size_t i = 0; i < materials.size(); ++i) {
        if (cancelled.load()) return result;

        auto& mat = *materials[i];

        if (progress) {
            progress({"Converting materials", getFilename(mat.unityPath),
                      (float)(i + 1) / (float)materials.size()});
        }

        // Read the .mat file
        std::ifstream file(mat.tempFilePath, std::ios::binary);
        if (!file) {
            log.error("Failed to read material file: " + mat.unityPath);
            continue;
        }

        std::ostringstream buf;
        buf << file.rdbuf();
        std::string content = buf.str();
        file.close();

        // Parse Unity YAML
        YamlFile yaml = parseUnityYaml(content);

        // Find the Material document (classID == 21)
        const YamlDocument* matDoc = nullptr;
        for (auto& doc : yaml.documents) {
            if (doc.classID == 21) {
                matDoc = &doc;
                break;
            }
        }

        if (!matDoc) {
            log.warn("No Material document found in: " + mat.unityPath);
            continue;
        }

        const YamlNode& root = matDoc->root;
        std::string matName = root["m_Name"].str("UnknownMaterial");

        // Detect shader type
        ShaderType shaderType = detectShader(root);

        // Build the .tres content
        TresBuilder tres;

        switch (shaderType) {
            case ShaderType::URPLit:
                buildURPLit(tres, root, guids, log);
                break;
            case ShaderType::URPUnlit:
                buildURPUnlit(tres, root, guids, log);
                break;
            case ShaderType::LegacyStandard:
                buildLegacyStandard(tres, root, guids, log);
                break;
            case ShaderType::Unknown:
                log.warn("Unknown shader for material '" + matName + "' (" + mat.unityPath
                         + "), creating default white material.");
                buildDefaultMaterial(tres);
                break;
        }

        std::string tresContent = tres.build();

        // Determine output path: Assets/Materials/Foo.mat -> Materials/Foo.tres
        std::string relPath = stripAssetsPrefix(mat.unityPath);
        relPath = replaceExtension(relPath, ".tres");
        std::string outPath = outputDir + "/" + relPath;

        // Create directories and write the file
        std::error_code ec;
        fs::create_directories(fs::path(outPath).parent_path(), ec);
        if (ec) {
            log.error("Failed to create directory for material: " + outPath + " - " + ec.message());
            continue;
        }

        std::ofstream outFile(outPath, std::ios::binary);
        if (!outFile) {
            log.error("Failed to write material file: " + outPath);
            continue;
        }
        outFile << tresContent;
        outFile.close();

        // Add to the material map: guid -> res:// path
        std::string resPath = "res://" + relPath;
        result[mat.guid] = resPath;

        log.info("Converted material: " + matName + " -> " + resPath);
    }

    log.info("Materials converted: " + std::to_string(result.size()) + "/" + std::to_string(materials.size()));
    return result;
}

} // namespace u2g
