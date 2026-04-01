#pragma once
#include "util/types.h"
#include "util/log.h"
#include "converter/unity_yaml_parser.h"
#include "converter/coord_convert.h"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <atomic>

namespace u2g {

// External resource reference in a .tscn/.tres file
struct ExtResource {
    std::string id;
    std::string type; // "PackedScene", "Material", "Texture2D"
    std::string path; // res:// path
};

// A node in the Godot scene tree
struct GodotNode {
    std::string name;
    std::string godotType;      // "Node3D", "DirectionalLight3D", etc.
    std::string transform;       // serialized Transform3D, or "" for identity
    std::string instanceResId;   // ext_resource id if this is an instance, else ""
    std::vector<std::pair<std::string, std::string>> properties; // key=value pairs
    std::vector<std::shared_ptr<GodotNode>> children;
    // FBX child-node overrides: (child_name, property, value)
    std::vector<std::tuple<std::string, std::string, std::string>> childOverrides;
};

// Fully built scene ready to write as .tscn
struct TscnData {
    std::shared_ptr<GodotNode> root;
    std::vector<ExtResource> extResources;
};

// Build Godot scene data from Unity YAML content.
// materialMap: guid -> res:// .tres path
// prefabMap:   guid -> res:// .tscn path
TscnData buildTscnData(const std::string& yamlContent,
                        const GuidTable& guids,
                        const std::map<std::string, std::string>& materialMap,
                        const std::map<std::string, std::string>& prefabMap,
                        const std::string& outputDir,
                        Log& log);

// Serialize TscnData to a .tscn file on disk.
void writeTscnFile(const std::string& outputPath, const TscnData& data);

// High-level: convert all .unity scene files.
void convertScenes(const GuidTable& guids,
                   const std::map<std::string, std::string>& materialMap,
                   const std::map<std::string, std::string>& prefabMap,
                   const std::string& outputDir,
                   Log& log,
                   std::atomic<bool>& cancelled,
                   ProgressCallback progress);

} // namespace u2g
