#include "converter/scene_converter.h"
#include "converter/light_converter.h"
#include "converter/camera_converter.h"
#include <ufbx.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>
#include <cassert>

namespace fs = std::filesystem;

namespace u2g {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Class IDs we care about.
constexpr int CID_GAME_OBJECT    = 1;
constexpr int CID_TRANSFORM      = 4;
constexpr int CID_CAMERA         = 20;
constexpr int CID_MESH_RENDERER  = 23;
constexpr int CID_RENDERER       = 25;
constexpr int CID_MESH_FILTER    = 33;
constexpr int CID_LIGHT          = 108;
constexpr int CID_PREFAB_INST    = 1001;
constexpr int CID_SCENE_ROOTS    = 1660057539;

// Intermediate object model built from Unity YAML documents.

struct UnityTransform {
    int64_t fileID = 0;
    int64_t gameObjectID = 0;
    int64_t fatherID = 0;
    std::vector<int64_t> childrenIDs;
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1, 1, 1};
};

struct UnityGameObject {
    int64_t fileID = 0;
    std::string name;
    std::vector<int64_t> componentIDs; // fileIDs of attached components
    int64_t transformID = 0;           // fileID of this GO's Transform
};

// Convenience: read a Vec3 from a YAML map with x,y,z keys.
Vec3 readVec3(const YamlNode& n) {
    return { n["x"].toFloat(0), n["y"].toFloat(0), n["z"].toFloat(0) };
}

// Read a Quat from a YAML map with x,y,z,w keys.
Quat readQuat(const YamlNode& n) {
    return { n["x"].toFloat(0), n["y"].toFloat(0), n["z"].toFloat(0), n["w"].toFloat(1) };
}

// Sanitize a node name for Godot (remove special chars that break .tscn paths).
std::string sanitizeName(const std::string& name) {
    if (name.empty()) return "Node";
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == ':' || c == '@') {
            out += '_';
        } else {
            out += c;
        }
    }
    return out;
}

// Make a node name unique among siblings. Modifies `usedNames` in place.
std::string makeUniqueName(const std::string& desired, std::set<std::string>& usedNames) {
    std::string base = desired;
    if (base.empty()) base = "Node";
    if (usedNames.find(base) == usedNames.end()) {
        usedNames.insert(base);
        return base;
    }
    // Append _2, _3, ... until unique
    for (int suffix = 2; ; ++suffix) {
        std::string candidate = base + "_" + std::to_string(suffix);
        if (usedNames.find(candidate) == usedNames.end()) {
            usedNames.insert(candidate);
            return candidate;
        }
    }
}

// Find or add an ExtResource, returning the id string.
std::string addExtResource(std::vector<ExtResource>& resources,
                           const std::string& type,
                           const std::string& path) {
    // Check if already present.
    for (auto& r : resources) {
        if (r.type == type && r.path == path)
            return r.id;
    }
    std::string id = std::to_string(resources.size() + 1);
    resources.push_back({id, type, path});
    return id;
}

// Get the Godot res:// path for an asset from its GUID.
std::string resPathForGuid(const std::string& guid, const GuidTable& guids) {
    auto it = guids.find(guid);
    if (it == guids.end()) return "";
    return "res://" + stripAssetsPrefix(it->second.unityPath);
}

// Get the disk path for an FBX within the output directory.
std::string fbxDiskPath(const std::string& guid, const GuidTable& guids,
                        const std::string& outputDir) {
    auto it = guids.find(guid);
    if (it == guids.end()) return "";
    return outputDir + "/" + stripAssetsPrefix(it->second.unityPath);
}

// Use ufbx to extract mesh node names from an FBX file.
// Returns a vector of mesh node names in order.
std::vector<std::string> extractFbxMeshNodeNames(const std::string& fbxPath) {
    std::vector<std::string> names;
    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(fbxPath.c_str(), NULL, &error);
    if (!scene) return names;

    for (size_t i = 0; i < scene->nodes.count; i++) {
        ufbx_node* node = scene->nodes.data[i];
        if (node->mesh) {
            names.emplace_back(node->name.data, node->name.length);
        }
    }
    ufbx_free_scene(scene);
    return names;
}

// ---------------------------------------------------------------------------
// Tree builder
// ---------------------------------------------------------------------------

struct SceneContext {
    // Document map: fileID -> pointer into parsed documents.
    std::map<int64_t, const YamlDocument*> docMap;

    // Object model.
    std::map<int64_t, UnityGameObject> gameObjects;
    std::map<int64_t, UnityTransform> transforms;

    // Lookup: which document is which component type (keyed by fileID).
    std::map<int64_t, int> classIDByFileID; // fileID -> classID

    // PrefabInstance documents (fileID -> doc).
    std::map<int64_t, const YamlDocument*> prefabInstances;

    // Scene roots (if SceneRoots doc exists).
    std::vector<int64_t> sceneRootIDs; // fileIDs of root GameObjects

    // External resource tracking.
    std::vector<ExtResource> extResources;

    // References.
    const GuidTable* guids = nullptr;
    const std::map<std::string, std::string>* materialMap = nullptr;
    const std::map<std::string, std::string>* prefabMap = nullptr;
    std::string outputDir;
    Log* log = nullptr;
};

void buildObjectModel(SceneContext& ctx) {
    // First pass: index all documents.
    for (auto& [fid, doc] : ctx.docMap) {
        ctx.classIDByFileID[fid] = doc->classID;
    }

    // Build GameObjects.
    for (auto& [fid, doc] : ctx.docMap) {
        if (doc->classID != CID_GAME_OBJECT) continue;
        auto& root = doc->root;
        UnityGameObject go;
        go.fileID = fid;
        go.name = root["m_Name"].str("GameObject");

        // m_Component is a list of {component: {fileID: X}}
        auto& compSeq = root["m_Component"];
        if (compSeq.isSeq()) {
            for (auto& item : compSeq.sequence) {
                auto& comp = item["component"];
                int64_t compID = comp["fileID"].toInt(0);
                if (compID != 0)
                    go.componentIDs.push_back(compID);
            }
        }
        ctx.gameObjects[fid] = std::move(go);
    }

    // Build Transforms.
    for (auto& [fid, doc] : ctx.docMap) {
        if (doc->classID != CID_TRANSFORM) continue;
        auto& root = doc->root;
        UnityTransform tr;
        tr.fileID = fid;
        tr.gameObjectID = root["m_GameObject"]["fileID"].toInt(0);
        tr.fatherID = root["m_Father"]["fileID"].toInt(0);
        tr.position = readVec3(root["m_LocalPosition"]);
        tr.rotation = readQuat(root["m_LocalRotation"]);
        tr.scale = readVec3(root["m_LocalScale"]);
        // Default scale to 1 if zero (some broken exports).
        if (tr.scale.x == 0 && tr.scale.y == 0 && tr.scale.z == 0) {
            tr.scale = {1, 1, 1};
        }

        auto& childrenSeq = root["m_Children"];
        if (childrenSeq.isSeq()) {
            for (auto& item : childrenSeq.sequence) {
                int64_t childID = item["fileID"].toInt(0);
                if (childID != 0)
                    tr.childrenIDs.push_back(childID);
            }
        }
        ctx.transforms[fid] = std::move(tr);
    }

    // Link GameObjects to their Transform component.
    for (auto& [fid, tr] : ctx.transforms) {
        if (tr.gameObjectID != 0) {
            auto goIt = ctx.gameObjects.find(tr.gameObjectID);
            if (goIt != ctx.gameObjects.end()) {
                goIt->second.transformID = fid;
            }
        }
    }

    // Collect PrefabInstances.
    for (auto& [fid, doc] : ctx.docMap) {
        if (doc->classID == CID_PREFAB_INST) {
            ctx.prefabInstances[fid] = doc;
        }
    }

    // Collect SceneRoots.
    for (auto& [fid, doc] : ctx.docMap) {
        if (doc->classID == CID_SCENE_ROOTS) {
            auto& roots = doc->root["m_Roots"];
            if (roots.isSeq()) {
                for (auto& item : roots.sequence) {
                    int64_t rootID = item["fileID"].toInt(0);
                    if (rootID != 0)
                        ctx.sceneRootIDs.push_back(rootID);
                }
            }
        }
    }
}

// Find the Transform fileID for a given GameObject fileID.
int64_t findTransformForGO(const SceneContext& ctx, int64_t goFileID) {
    auto goIt = ctx.gameObjects.find(goFileID);
    if (goIt == ctx.gameObjects.end()) return 0;
    return goIt->second.transformID;
}

// Find a component of a specific classID attached to a GameObject.
const YamlDocument* findComponent(const SceneContext& ctx, const UnityGameObject& go, int classID) {
    for (int64_t compID : go.componentIDs) {
        auto cidIt = ctx.classIDByFileID.find(compID);
        if (cidIt != ctx.classIDByFileID.end() && cidIt->second == classID) {
            auto docIt = ctx.docMap.find(compID);
            if (docIt != ctx.docMap.end())
                return docIt->second;
        }
    }
    return nullptr;
}

// Convert a single Transform + its associated GameObject into a GodotNode,
// then recurse into children.
std::shared_ptr<GodotNode> convertNode(SceneContext& ctx,
                                        const UnityTransform& transform) {
    auto node = std::make_shared<GodotNode>();

    // Find the associated GameObject.
    auto goIt = ctx.gameObjects.find(transform.gameObjectID);
    std::string rawName = "Node3D";
    const UnityGameObject* go = nullptr;
    if (goIt != ctx.gameObjects.end()) {
        go = &goIt->second;
        rawName = go->name;
    }
    node->name = sanitizeName(rawName);
    node->godotType = "Node3D";

    // Convert transform.
    Transform3D t = unityToGodot(transform.position, transform.rotation, transform.scale);
    if (!isIdentity(t)) {
        node->transform = serializeTransform(t);
    }

    // Determine the Godot type based on attached components.
    if (go) {
        // Check for MeshFilter + MeshRenderer => FBX instance.
        auto* meshFilterDoc = findComponent(ctx, *go, CID_MESH_FILTER);
        auto* meshRendererDoc = findComponent(ctx, *go, CID_MESH_RENDERER);
        if (!meshRendererDoc)
            meshRendererDoc = findComponent(ctx, *go, CID_RENDERER);

        if (meshFilterDoc && meshRendererDoc) {
            // Get mesh GUID.
            auto& meshRef = meshFilterDoc->root["m_Mesh"];
            std::string meshGuid = meshRef["guid"].str();

            if (!meshGuid.empty()) {
                std::string meshResPath = resPathForGuid(meshGuid, *ctx.guids);
                if (!meshResPath.empty()) {
                    // Add as PackedScene external resource for FBX instancing.
                    std::string resId = addExtResource(ctx.extResources, "PackedScene", meshResPath);
                    node->instanceResId = resId;
                    // When instancing, godotType is not emitted.
                    node->godotType.clear();

                    // Material overrides.
                    auto& matSeq = meshRendererDoc->root["m_Materials"];
                    if (matSeq.isSeq()) {
                        // Try to get FBX mesh node names for proper override paths.
                        std::string diskPath = fbxDiskPath(meshGuid, *ctx.guids, ctx.outputDir);
                        std::vector<std::string> meshNodeNames;
                        if (!diskPath.empty() && fs::exists(diskPath)) {
                            meshNodeNames = extractFbxMeshNodeNames(diskPath);
                        }

                        for (size_t mi = 0; mi < matSeq.sequence.size(); ++mi) {
                            auto& matRef = matSeq.sequence[mi];
                            std::string matGuid = matRef["guid"].str();
                            if (matGuid.empty()) continue;

                            auto matIt = ctx.materialMap->find(matGuid);
                            if (matIt == ctx.materialMap->end()) continue;

                            std::string matResId = addExtResource(ctx.extResources, "Material", matIt->second);

                            if (!meshNodeNames.empty()) {
                                // Use the first mesh node name for the override path.
                                // Godot FBX import creates child nodes named after
                                // the mesh nodes in the FBX.
                                std::string childName = meshNodeNames[0];
                                node->childOverrides.push_back({
                                    childName,
                                    "surface_material_override/" + std::to_string(mi),
                                    "ExtResource(\"" + matResId + "\")"
                                });
                            } else {
                                // Fallback: apply material override directly on the
                                // instanced node (works when the FBX has a single mesh
                                // at root level).
                                node->properties.push_back({
                                    "surface_material_override/" + std::to_string(mi),
                                    "ExtResource(\"" + matResId + "\")"
                                });
                            }
                        }
                    }
                } else {
                    ctx.log->warn("Mesh GUID " + meshGuid + " not found in GUID table for: " + rawName);
                }
            }
        }

        // Check for Light component.
        auto* lightDoc = findComponent(ctx, *go, CID_LIGHT);
        if (lightDoc && node->instanceResId.empty()) {
            LightData ld = convertLight(lightDoc->root);
            node->godotType = ld.godotType;

            // Color.
            std::string colorStr = "Color(" + fmtFloat(ld.r) + ", " + fmtFloat(ld.g) + ", "
                                   + fmtFloat(ld.b) + ", 1)";
            node->properties.push_back({"light_color", colorStr});
            node->properties.push_back({"light_energy", fmtFloat(ld.energy)});

            if (ld.godotType == "OmniLight3D") {
                node->properties.push_back({"omni_range", fmtFloat(ld.range)});
            } else if (ld.godotType == "SpotLight3D") {
                node->properties.push_back({"spot_range", fmtFloat(ld.range)});
                node->properties.push_back({"spot_angle", fmtFloat(ld.spotAngle)});
            }

            if (ld.shadowEnabled) {
                node->properties.push_back({"shadow_enabled", "true"});
            }
        }

        // Check for Camera component.
        auto* cameraDoc = findComponent(ctx, *go, CID_CAMERA);
        if (cameraDoc && node->instanceResId.empty() && node->godotType == "Node3D") {
            CameraData cd = convertCamera(cameraDoc->root);
            node->godotType = "Camera3D";

            if (cd.orthographic) {
                node->properties.push_back({"projection", "1"}); // orthogonal
                node->properties.push_back({"size", fmtFloat(cd.orthoSize * 2.0f)});
            } else {
                node->properties.push_back({"fov", fmtFloat(cd.fov)});
            }
            node->properties.push_back({"near", fmtFloat(cd.nearClip)});
            node->properties.push_back({"far", fmtFloat(cd.farClip)});
        }
    }

    // Recurse into children.
    std::set<std::string> usedChildNames;
    for (int64_t childTransformID : transform.childrenIDs) {
        auto childIt = ctx.transforms.find(childTransformID);
        if (childIt == ctx.transforms.end()) continue;
        auto child = convertNode(ctx, childIt->second);
        if (child) {
            child->name = makeUniqueName(child->name, usedChildNames);
            node->children.push_back(std::move(child));
        }
    }

    return node;
}

// Convert a PrefabInstance document into a GodotNode.
std::shared_ptr<GodotNode> convertPrefabInstance(SceneContext& ctx,
                                                  const YamlDocument* doc) {
    auto& root = doc->root;
    auto& modification = root["m_Modification"];
    auto& sourcePrefab = root["m_SourcePrefab"];

    std::string prefabGuid = sourcePrefab["guid"].str();
    if (prefabGuid.empty()) {
        ctx.log->warn("PrefabInstance has no m_SourcePrefab guid, skipping.");
        return nullptr;
    }

    // Look up the prefab in the prefab map.
    auto prefabIt = ctx.prefabMap->find(prefabGuid);
    if (prefabIt == ctx.prefabMap->end()) {
        ctx.log->warn("PrefabInstance references unknown prefab GUID: " + prefabGuid);
        // Create a placeholder Node3D.
        auto node = std::make_shared<GodotNode>();
        node->name = "MissingPrefab";
        node->godotType = "Node3D";
        return node;
    }

    auto node = std::make_shared<GodotNode>();
    std::string resId = addExtResource(ctx.extResources, "PackedScene", prefabIt->second);
    node->instanceResId = resId;

    // Determine the node name from the prefab path (stem of the .tscn).
    std::string prefabName = getStem(prefabIt->second);
    node->name = sanitizeName(prefabName);

    // Process m_Modifications to extract transform and material overrides.
    Vec3 pos{0, 0, 0};
    Quat rot{0, 0, 0, 1};
    Vec3 scl{1, 1, 1};
    bool hasPos = false, hasRot = false, hasScl = false;

    auto& mods = modification["m_Modifications"];
    if (mods.isSeq()) {
        for (auto& mod : mods.sequence) {
            std::string propPath = mod["propertyPath"].str();
            std::string value = mod["value"].str();

            // Safe float parse — returns 0 on failure.
            auto safeFloat = [](const std::string& s) -> float {
                try { return std::stof(s); } catch (...) { return 0.0f; }
            };

            // Transform overrides.
            if (propPath == "m_LocalPosition.x") { pos.x = safeFloat(value); hasPos = true; }
            else if (propPath == "m_LocalPosition.y") { pos.y = safeFloat(value); hasPos = true; }
            else if (propPath == "m_LocalPosition.z") { pos.z = safeFloat(value); hasPos = true; }
            else if (propPath == "m_LocalRotation.x") { rot.x = safeFloat(value); hasRot = true; }
            else if (propPath == "m_LocalRotation.y") { rot.y = safeFloat(value); hasRot = true; }
            else if (propPath == "m_LocalRotation.z") { rot.z = safeFloat(value); hasRot = true; }
            else if (propPath == "m_LocalRotation.w") { rot.w = safeFloat(value); hasRot = true; }
            else if (propPath == "m_LocalScale.x") { scl.x = safeFloat(value); hasScl = true; }
            else if (propPath == "m_LocalScale.y") { scl.y = safeFloat(value); hasScl = true; }
            else if (propPath == "m_LocalScale.z") { scl.z = safeFloat(value); hasScl = true; }
            // Material overrides: m_Materials.Array.data[N]
            else if (propPath.find("m_Materials.Array.data[") == 0) {
                // Extract the material reference from objectReference.
                auto& objRef = mod["objectReference"];
                std::string matGuid = objRef["guid"].str();
                if (!matGuid.empty()) {
                    auto matIt = ctx.materialMap->find(matGuid);
                    if (matIt != ctx.materialMap->end()) {
                        // Extract index from property path.
                        size_t bracket = propPath.find('[');
                        size_t bracket2 = propPath.find(']');
                        if (bracket != std::string::npos && bracket2 != std::string::npos) {
                            std::string idxStr = propPath.substr(bracket + 1, bracket2 - bracket - 1);
                            std::string matResId = addExtResource(ctx.extResources, "Material", matIt->second);
                            node->properties.push_back({
                                "surface_material_override/" + idxStr,
                                "ExtResource(\"" + matResId + "\")"
                            });
                        }
                    }
                }
            }
            // m_Name override.
            else if (propPath == "m_Name") {
                if (!value.empty()) {
                    node->name = sanitizeName(value);
                }
            }
        }
    }

    // Apply transform.
    if (hasPos || hasRot || hasScl) {
        Transform3D t = unityToGodot(pos, rot, scl);
        if (!isIdentity(t)) {
            node->transform = serializeTransform(t);
        }
    }

    return node;
}

// ---------------------------------------------------------------------------
// .tscn writer helpers
// ---------------------------------------------------------------------------

// Recursively write nodes with correct parent paths.
void writeNodeRecursive(std::ostringstream& out,
                        const GodotNode& node,
                        const std::string& parentPath,
                        bool isRoot) {
    // Write node header line.
    out << "\n[node name=\"" << node.name << "\"";

    if (isRoot) {
        // Root node: type only, no parent.
        out << " type=\"Node3D\"";
    } else {
        if (!parentPath.empty()) {
            out << " parent=\"" << parentPath << "\"";
        }
        if (!node.instanceResId.empty()) {
            out << " instance=ExtResource(\"" << node.instanceResId << "\")";
        } else {
            out << " type=\"" << (node.godotType.empty() ? "Node3D" : node.godotType) << "\"";
        }
    }

    out << "]\n";

    // Write transform if present.
    if (!node.transform.empty()) {
        out << "transform = " << node.transform << "\n";
    }

    // Write properties.
    for (auto& [key, val] : node.properties) {
        out << key << " = " << val << "\n";
    }

    // Write child overrides (for instanced FBX nodes).
    // These appear as separate [node] blocks referencing children of the instanced scene.
    // We collect them and write after the current node but as part of the recursive walk.
    // (We handle them after writing this node's direct children.)

    // Determine the path for children of this node.
    std::string childParentPath;
    if (isRoot) {
        childParentPath = ".";
    } else if (parentPath == ".") {
        childParentPath = node.name;
    } else {
        childParentPath = parentPath + "/" + node.name;
    }

    // Write child override nodes (for FBX material overrides on sub-nodes).
    for (auto& [childName, prop, val] : node.childOverrides) {
        out << "\n[node name=\"" << childName << "\" parent=\"" << childParentPath << "\"]\n";
        out << prop << " = " << val << "\n";
    }

    // Recurse into children.
    for (auto& child : node.children) {
        writeNodeRecursive(out, *child, childParentPath, false);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public: buildTscnData
// ---------------------------------------------------------------------------

TscnData buildTscnData(const std::string& yamlContent,
                        const GuidTable& guids,
                        const std::map<std::string, std::string>& materialMap,
                        const std::map<std::string, std::string>& prefabMap,
                        const std::string& outputDir,
                        Log& log) {
    TscnData result;

    // Step 1: Parse.
    YamlFile yamlFile = parseUnityYaml(yamlContent);
    if (yamlFile.documents.empty()) {
        log.warn("YAML file contains no documents.");
        return result;
    }

    // Step 2: Build context.
    SceneContext ctx;
    ctx.guids = &guids;
    ctx.materialMap = &materialMap;
    ctx.prefabMap = &prefabMap;
    ctx.outputDir = outputDir;
    ctx.log = &log;

    for (auto& doc : yamlFile.documents) {
        ctx.docMap[doc.fileID] = &doc;
    }

    buildObjectModel(ctx);

    // Step 3: Find root transforms.
    std::vector<int64_t> rootTransformIDs;

    if (!ctx.sceneRootIDs.empty()) {
        // Unity 2022+ SceneRoots document lists root GameObjects.
        for (int64_t goID : ctx.sceneRootIDs) {
            int64_t trID = findTransformForGO(ctx, goID);
            if (trID != 0)
                rootTransformIDs.push_back(trID);
        }
    }

    if (rootTransformIDs.empty()) {
        // Fallback: find all Transforms whose m_Father is 0 or not found.
        for (auto& [fid, tr] : ctx.transforms) {
            if (tr.fatherID == 0 || ctx.transforms.find(tr.fatherID) == ctx.transforms.end()) {
                rootTransformIDs.push_back(fid);
            }
        }
    }

    // Also collect PrefabInstances whose m_TransformParent is 0 (top-level).
    std::vector<const YamlDocument*> rootPrefabInstances;
    for (auto& [fid, doc] : ctx.prefabInstances) {
        auto& mod = doc->root["m_Modification"];
        int64_t parentID = mod["m_TransformParent"]["fileID"].toInt(0);
        if (parentID == 0) {
            rootPrefabInstances.push_back(doc);
        }
    }

    // Step 4: Build the Godot tree.
    // Create a root Node3D that encompasses the scene.
    auto godotRoot = std::make_shared<GodotNode>();
    godotRoot->name = "Scene";
    godotRoot->godotType = "Node3D";

    std::set<std::string> usedRootNames;

    // If there is exactly one root transform and no root prefab instances,
    // use it as the root node directly.
    if (rootTransformIDs.size() == 1 && rootPrefabInstances.empty()) {
        auto trIt = ctx.transforms.find(rootTransformIDs[0]);
        if (trIt != ctx.transforms.end()) {
            auto converted = convertNode(ctx, trIt->second);
            if (converted) {
                godotRoot = converted;
            }
        }
    } else {
        // Multiple roots: wrap in a Scene node.
        for (int64_t trID : rootTransformIDs) {
            auto trIt = ctx.transforms.find(trID);
            if (trIt == ctx.transforms.end()) continue;
            auto child = convertNode(ctx, trIt->second);
            if (child) {
                child->name = makeUniqueName(child->name, usedRootNames);
                godotRoot->children.push_back(std::move(child));
            }
        }

        // Add prefab instances as children of the root.
        for (auto* piDoc : rootPrefabInstances) {
            auto child = convertPrefabInstance(ctx, piDoc);
            if (child) {
                child->name = makeUniqueName(child->name, usedRootNames);
                godotRoot->children.push_back(std::move(child));
            }
        }
    }

    // Handle prefab instances that are children of existing transforms.
    // These should have been caught by the parent transform's children list,
    // but PrefabInstances in Unity don't have a normal Transform in the
    // children list — they use m_TransformParent instead. We need to attach
    // them to the right parent.
    // For now, any non-root prefab instances that we haven't placed yet
    // get attached to the root.
    for (auto& [fid, doc] : ctx.prefabInstances) {
        auto& mod = doc->root["m_Modification"];
        int64_t parentID = mod["m_TransformParent"]["fileID"].toInt(0);
        if (parentID != 0) {
            // This is a child prefab instance. For simplicity in V1,
            // we attach it to the root. A more complete implementation
            // would walk the tree to find the correct parent.
            auto child = convertPrefabInstance(ctx, doc);
            if (child) {
                child->name = makeUniqueName(child->name, usedRootNames);
                godotRoot->children.push_back(std::move(child));
            }
        }
    }

    result.root = godotRoot;
    result.extResources = std::move(ctx.extResources);
    return result;
}

// ---------------------------------------------------------------------------
// Public: writeTscnFile
// ---------------------------------------------------------------------------

void writeTscnFile(const std::string& outputPath, const TscnData& data) {
    if (!data.root) return;

    std::ostringstream out;

    // Header.
    int loadSteps = (int)data.extResources.size() + 1;
    out << "[gd_scene load_steps=" << loadSteps << " format=3]\n";

    // External resources.
    for (auto& ext : data.extResources) {
        out << "\n[ext_resource type=\"" << ext.type
            << "\" path=\"" << ext.path
            << "\" id=\"" << ext.id << "\"]\n";
    }

    // Nodes.
    writeNodeRecursive(out, *data.root, "", true);

    // Write to disk.
    std::ofstream f(outputPath, std::ios::binary);
    if (f) {
        std::string content = out.str();
        f.write(content.data(), (std::streamsize)content.size());
    }
}

// ---------------------------------------------------------------------------
// Public: convertScenes
// ---------------------------------------------------------------------------

void convertScenes(const GuidTable& guids,
                   const std::map<std::string, std::string>& materialMap,
                   const std::map<std::string, std::string>& prefabMap,
                   const std::string& outputDir,
                   Log& log,
                   std::atomic<bool>& cancelled,
                   ProgressCallback progress) {
    // Collect scene entries.
    std::vector<const AssetEntry*> scenes;
    for (auto& [guid, entry] : guids) {
        if (entry.type == AssetType::Scene && !entry.tempFilePath.empty())
            scenes.push_back(&entry);
    }

    if (scenes.empty()) {
        log.info("No scenes to convert.");
        return;
    }

    log.info("Converting " + std::to_string(scenes.size()) + " scenes...");

    for (size_t i = 0; i < scenes.size(); ++i) {
        if (cancelled.load()) return;

        auto& entry = *scenes[i];

        if (progress) {
            progress({"Converting scenes", getFilename(entry.unityPath),
                      (float)(i + 1) / (float)scenes.size()});
        }

        // Read the YAML file.
        std::ifstream f(entry.tempFilePath, std::ios::binary);
        if (!f) {
            log.error("Failed to read scene: " + entry.unityPath);
            continue;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        // Build the scene data.
        TscnData data = buildTscnData(ss.str(), guids, materialMap, prefabMap, outputDir, log);
        if (!data.root) {
            log.error("Failed to build scene: " + entry.unityPath);
            continue;
        }

        // Determine output path.
        std::string relPath = replaceExtension(stripAssetsPrefix(entry.unityPath), ".tscn");
        std::string outPath = outputDir + "/" + relPath;

        fs::create_directories(fs::path(outPath).parent_path());

        // Set the root node name from the scene filename.
        data.root->name = sanitizeName(getStem(entry.unityPath));

        writeTscnFile(outPath, data);

        log.info("Converted scene: " + entry.unityPath + " -> " + relPath);
    }

    log.info("Scenes converted: " + std::to_string(scenes.size()));
}

} // namespace u2g
