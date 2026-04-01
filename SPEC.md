# Unity2Godot Converter — V1 Specification

## Overview

A cross-platform desktop tool that converts a `.unitypackage` file into a ready-to-open Godot 4.6.1 project. V1 is limited to static meshes (FBX), textures, materials (URP + legacy), and scenes. No C# scripts, custom shaders, skinned meshes, animations, or audio.

**Target Godot version:** 4.6.1

---

## Architecture

### Language & Build

- **C++17**, built with **CMake** (minimum 3.16)
- All dependencies vendored in `thirdparty/`
- Cross-platform: **Windows, macOS, Linux**

### Dependencies (all vendored in `thirdparty/`)

| Library | Purpose |
|---|---|
| **ufbx** | Parse FBX files to extract node/mesh names for material override paths and fileID resolution |
| **Dear ImGui** | GUI framework (immediate mode) |
| **GLFW** | Windowing/input backend for ImGui |
| **OpenGL3** | Rendering backend for ImGui (system-provided) |
| **nativefiledialog-extended** | Native OS file/folder picker dialogs |
| **miniz** | gzip/deflate decompression for .unitypackage |

---

## Conversion Pipeline

### High-Level Flow

```
.unitypackage (tar.gz)
    │
    ▼
[1. Extract to temp dir]
    │
    ▼
[2. Build GUID → path table from all pathname + asset.meta files]
    │
    ▼
[3. Classify assets by type: scene, prefab, material, texture, FBX, other]
    │
    ▼
[4. Convert each asset type (order matters)]
    │   a. Textures → copy to output
    │   b. FBX → copy to output (Godot imports natively)
    │   c. Materials → convert to .tres files
    │   d. Prefabs → convert to .tscn files
    │   e. Scenes → convert to .tscn files
    │
    ▼
[5. Generate project.godot + folder structure]
    │
    ▼
[6. Produce skip report + cleanup temp dir]
```

### Threading Model

- Conversion runs on a **background worker thread**
- Progress callbacks update the ImGui UI on the main thread
- The worker reports: current phase, asset name, progress percentage, warnings/errors
- Cancellation is supported via an atomic flag checked between assets

---

## 1. Package Extraction

### Input Format

`.unitypackage` is a **tar.gz** archive. Inside, each asset is stored in a GUID-named folder:

```
<guid>/
    pathname    — text file containing the asset's Unity project path (e.g., "Assets/Models/building.fbx")
    asset       — the actual file data
    asset.meta  — Unity YAML with import settings, GUID declaration, etc.
    preview.png — optional thumbnail (ignored)
```

### Extraction Process

1. Decompress gzip layer using **miniz**
2. Read tar entries using a **custom tar reader** (~100 lines; tar is a trivial format of 512-byte header blocks followed by raw data)
3. Extract all entries to a **temporary directory**
4. Build GUID → path mapping table by reading every `pathname` file
5. Parse every `asset.meta` file to extract:
   - GUID (redundant with folder name but validates)
   - Asset type / importer type
   - Import settings (texture settings, FBX settings, etc.)
6. Temp directory is deleted after conversion completes (or on cancellation/error)

### Edge Cases

- **Prefab-only packages:** Some `.unitypackage` files contain only prefabs, textures, and models with no scenes. The converter handles this gracefully — all assets are converted normally, no `.tscn` scene files are produced, and `project.godot` is still generated. This is a valid output.
- **Missing `asset` data:** Some entries in a `.unitypackage` have a `pathname` and `asset.meta` but no `asset` file (e.g., folder entries, or assets excluded during export). These entries are added to the GUID table (for path resolution) but skipped during conversion with no warning — this is normal.
- **Path separators:** The `pathname` file may contain forward slashes or backslashes depending on the OS that created the package. All paths are normalized to forward slashes during extraction.

---

## 2. GUID Resolution

### GUID Table

A complete in-memory map is built **upfront** before any conversion begins:

```
Map<string, AssetEntry> guidTable;

struct AssetEntry {
    string guid;
    string unityPath;       // e.g., "Assets/Textures/brick_albedo.png"
    string tempFilePath;    // path to extracted asset data
    AssetType type;         // texture, fbx, material, scene, prefab, script, other
    MetaData meta;          // parsed import settings from .meta
};
```

### GUID + fileID References

Unity references assets via `{fileID: <id>, guid: <guid>, type: <type>}`. Resolution:

- **guid** → look up in the GUID table to find the asset
- **fileID** → identifies a sub-object within the asset (e.g., a specific mesh within an FBX, or a specific component). For FBX files, fileID is a deterministic hash of the sub-object name.
- **type** → 2 = native Unity asset, 3 = external asset (FBX, texture, etc.)

---

## 3. Unity YAML Parser

### Approach: Custom Lightweight Parser

Unity scene/prefab/material files use a **non-standard YAML 1.1 variant** with custom tags. Rather than fighting a YAML library, we build a purpose-built parser.

### Unity YAML Structure

```yaml
%YAML 1.1
%TAG !u! tag:unity3d.com,2011:
--- !u!1 &123456
GameObject:
  m_Name: MyObject
  m_Component:
  - component: {fileID: 123457}
--- !u!4 &123457
Transform:
  m_LocalPosition: {x: 1, y: 2, z: 3}
  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}
  m_LocalScale: {x: 1, y: 1, z: 1}
  m_Children:
  - {fileID: 234567}
  m_Father: {fileID: 0}
```

### Parser Requirements

The parser needs to handle:

- `--- !u!<classID> &<fileID>` document separators → extract classID and local fileID
- Nested key-value mappings (indentation-based)
- Inline flow mappings: `{x: 1, y: 2, z: 3}`
- Inline flow sequences: `- {fileID: 123}`
- Multi-document files (scenes contain many `---` separated documents)
- String values (quoted and unquoted)
- Numeric values (int, float, including scientific notation)

### Unity Class IDs We Need

| ClassID | Unity Type | Purpose |
|---|---|---|
| 1 | GameObject | Scene hierarchy node |
| 4 | Transform | Position, rotation, scale, parent-child |
| 21 | Material | Material properties and shader reference |
| 23 | MeshRenderer | Material assignments on a mesh |
| 25 | Renderer | Base renderer (material list) |
| 33 | MeshFilter | Mesh asset reference |
| 108 | Light | Light type, color, intensity, range |
| 20 | Camera | FOV, near/far clip, projection type |
| 1001 | PrefabInstance | Prefab instantiation + overrides |
| 1660057539 | SceneRoots | Root object list (Unity 2022+) |

---

## 4. Texture Handling

### Copy & Transcode

1. **Supported by Godot natively:** PNG, JPG, WebP, BMP, TGA → **copy as-is**
2. **Not supported:** PSD, EXR → **copy as-is with a warning** that Godot cannot import these formats. User must convert manually. Transcoding support planned for a future version.

### Import Settings

No `.import` files are generated. Godot auto-generates these on first project open. Texture import settings from Unity `.meta` files (normal map detection, filter/wrap modes, sRGB) are not carried over in V1 — the user must configure these manually in Godot. Automatic import setting mapping is planned for a future version.

---

## 5. FBX Handling

### Approach: Keep FBX, Let Godot Import

FBX files are **copied directly** to the Godot project, preserving the original Unity folder structure (minus the `Assets/` prefix). Godot 4.6.1 natively imports FBX files.

### ufbx Usage

The converter uses **ufbx** to parse each FBX file during conversion for:

1. **Node/mesh name extraction** — read the names of all nodes and meshes inside the FBX, used to construct material override paths in `.tscn` files
2. **fileID resolution** — map Unity's deterministic fileID hashes to specific mesh/node names within the FBX
3. **Validation** — detect corrupt or unreadable FBX files early, before Godot attempts to import them

Since Godot 4.6.1 also uses ufbx internally for FBX import, the node names extracted by our converter should match the names Godot creates during import in most cases (see limitations).

### Scene References to FBX

When a Unity scene instances a mesh from an FBX file, the Godot scene will use `instance = ExtResource(...)` to load the entire FBX as a sub-scene at the correct transform. Individual mesh selection within the FBX is not performed — the whole model is instanced.

### Material Overrides on FBX Instances

When a Unity MeshRenderer assigns materials to a mesh from an FBX, the converter applies those materials as overrides on the instanced FBX's child nodes:

1. Use ufbx to read the FBX and extract node/mesh names
2. Map the Unity MeshRenderer's target to the corresponding node name in the FBX
3. Write child node overrides in the `.tscn` using that name as the path:

```ini
[node name="Building" parent="." instance=ExtResource("1")]

[node name="Wall" parent="Building"]
surface_material_override/0 = ExtResource("2")
```

This works when Godot's import produces the same node names as ufbx reports (~80-90% of cases). It can fail if Godot renames nodes (e.g., duplicate name suffixes, sanitization) or creates a different hierarchy structure. Failed overrides are logged as warnings.

### Multiple Sub-Object References

Unity references individual meshes inside an FBX via `{fileID, guid}` where the fileID identifies a specific sub-object (e.g., "Wall", "Roof", "Door" within `building.fbx`). There are two usage patterns:

1. **FBX placed via PrefabInstance** (most common) — the scene has a `PrefabInstance` component pointing to the FBX. Handled by prefab conversion — the whole model is instanced, which is correct.

2. **Individual meshes placed directly** (uncommon) — the scene has bare `MeshFilter` + `MeshRenderer` GameObjects, each referencing a different sub-mesh from the same FBX. Instancing the whole FBX at each location produces visual duplicates.

**V1 behavior:**

- During scene conversion, track all `(guid, fileID)` pairs that reference FBX files. Use ufbx to resolve fileIDs to human-readable mesh names.
- **Single unique fileID per FBX** (or single-mesh FBX): instance the whole FBX normally. This covers the vast majority of cases.
- **Multiple different fileIDs from the same FBX**: instance the whole FBX at each location anyway, but log a **prominent warning** listing the FBX file and which sub-objects were referenced by name, so the user knows to fix it manually in Godot.

---

## 6. Material Conversion

### Output Naming

Unity `.mat` files are converted to Godot `.tres` files using the original material filename with the extension changed. The original folder structure is preserved (via the `Assets/` prefix stripping rule in section 9), which naturally avoids name collisions between materials with the same name in different folders:

```
Assets/Environment/Materials/Glass.mat  →  Environment/Materials/Glass.tres
Assets/Characters/Materials/Glass.mat   →  Characters/Materials/Glass.tres
```

### Shader Scope

Three shader families are supported:

| Unity Shader | Godot Material | Notes |
|---|---|---|
| `Universal Render Pipeline/Lit` | `StandardMaterial3D` | Full PBR mapping |
| `Universal Render Pipeline/Unlit` | `StandardMaterial3D` (unshaded) | `shading_mode = SHADING_MODE_UNSHADED` |
| `Standard` (Built-in) | `StandardMaterial3D` | Legacy Built-in pipeline, full PBR |

Unknown/unsupported shaders → create a default white `StandardMaterial3D` + log a warning.

### Property Mapping: URP/Lit → StandardMaterial3D

| Unity Property | Godot Property | Conversion |
|---|---|---|
| `_BaseColor` | `albedo_color` | Direct RGBA mapping |
| `_BaseMap` | `albedo_texture` | Texture reference via GUID |
| `_Metallic` | `metallic` | Direct float |
| `_MetallicGlossMap` | `metallic_texture` | Texture reference |
| `_Smoothness` | `roughness` | `roughness = 1.0 - smoothness` |
| `_BumpMap` | `normal_texture` | Texture reference |
| `_BumpScale` | `normal_scale` | Direct float |
| `_EmissionColor` | `emission` + `emission_energy` | Color → emission, brightness → energy |
| `_EmissionMap` | `emission_texture` | Texture reference |
| `_OcclusionMap` | `ao_texture` | Texture reference |
| `_OcclusionStrength` | `ao_light_affect` | Direct float |
| `_Cutoff` | `alpha_scissor_threshold` | Alpha cutoff value |
| `_Surface` (0/1) | `transparency` | 0 = opaque, 1 = alpha |
| `_Cull` (0/1/2) | `cull_mode` | 0 = off, 1 = front, 2 = back |
| `_MainTex_ST` | `uv1_scale` + `uv1_offset` | `{x, y}` = scale, `{z, w}` = offset |

### Property Mapping: Legacy Standard → StandardMaterial3D

| Unity Property | Godot Property | Conversion |
|---|---|---|
| `_Color` | `albedo_color` | Direct RGBA |
| `_MainTex` | `albedo_texture` | Texture reference |
| `_MetallicGlossMap` | `metallic_texture` | Texture reference |
| `_Glossiness` | `roughness` | `roughness = 1.0 - glossiness` |
| `_BumpMap` | `normal_texture` | Texture reference |
| `_EmissionColor` | `emission` | Color mapping |
| `_EmissionMap` | `emission_texture` | Texture reference |
| `_OcclusionMap` | `ao_texture` | Texture reference |

### Output Format: .tres

```ini
[gd_resource type="StandardMaterial3D" format=3]

[ext_resource type="Texture2D" path="res://Textures/brick_albedo.png" id="1"]
[ext_resource type="Texture2D" path="res://Textures/brick_normal.png" id="2"]

[resource]
albedo_color = Color(1, 1, 1, 1)
albedo_texture = ExtResource("1")
metallic = 0.5
roughness = 0.5
normal_enabled = true
normal_texture = ExtResource("2")
```

---

## 7. Scene Conversion

### Unity Scene → Godot .tscn

Each `.unity` scene file is converted to a `.tscn` file.

### Duplicate Node Names

Unity allows sibling GameObjects with identical names. Godot does not — sibling node names must be unique. When building the Godot node tree, track sibling names at each hierarchy level. If a name already exists, append a numeric suffix:

```
Chair  →  Chair
Chair  →  Chair_2
Chair  →  Chair_3
```

Note: if a renamed node is the target of a prefab override (referenced by its original Unity name), the override may not resolve correctly. This is logged as a warning.

### Process

1. Parse the Unity scene file using the custom YAML parser
2. Build an in-memory tree of GameObjects from Transform parent-child relationships
3. For each GameObject, determine its Godot node type:
   - Has `MeshFilter` + `MeshRenderer` → instance the referenced FBX as `ExtResource`
   - Has `Light` component → `DirectionalLight3D`, `OmniLight3D`, or `SpotLight3D`
   - Has `Camera` component → `Camera3D`
   - Otherwise (empty/grouping) → `Node3D`
4. Apply coordinate system conversion to all transforms
5. Resolve material references: MeshRenderer's material list → ExtResource refs to converted .tres files
6. Write the .tscn file

### Coordinate System Conversion

Unity is **left-handed** (Y-up, Z-forward). Godot is **right-handed** (Y-up, -Z-forward).

Conversion applied to **scene-level transforms only** (FBX files are handled by Godot's importer):

```
godot_position.x =  unity_position.x
godot_position.y =  unity_position.y
godot_position.z = -unity_position.z

godot_rotation.x = -unity_rotation.x
godot_rotation.y = -unity_rotation.y
godot_rotation.z =  unity_rotation.z
godot_rotation.w =  unity_rotation.w
```

Scale is copied as-is (scale is handedness-independent).

### Transform3D Serialization

Godot's `Transform3D` is a 3x4 matrix: 3x3 basis (rotation + scale) followed by the origin (position). Unity stores transforms as separate position (Vector3), rotation (Quaternion), and scale (Vector3). The conversion is:

1. Convert the quaternion to a 3x3 rotation matrix (basis)
2. Multiply basis columns by the scale components
3. Write as `Transform3D(bx.x, by.x, bz.x, bx.y, by.y, bz.y, bx.z, by.z, bz.z, ox, oy, oz)`

Where `bx/by/bz` are the basis column vectors and `ox/oy/oz` is the origin.

**Identity transform omission:** Godot omits the `transform` property when it equals the identity (`Transform3D(1,0,0, 0,1,0, 0,0,1, 0,0,0)`). The converter should do the same — only write the `transform` property when it differs from identity. This keeps `.tscn` files clean.

### Node Type Mapping

| Unity Component | Godot Node | Property Mapping |
|---|---|---|
| GameObject (empty) | `Node3D` | name, transform |
| MeshFilter + MeshRenderer | `Node3D` + instanced FBX scene | transform, material overrides |
| Light (Directional) | `DirectionalLight3D` | color, intensity, shadows |
| Light (Point) | `OmniLight3D` | color, intensity, range, shadows |
| Light (Spot) | `SpotLight3D` | color, intensity, range, angle, shadows |
| Camera | `Camera3D` | fov, near, far, projection |

### Light Conversion Details

| Unity Property | Godot Property | Conversion |
|---|---|---|
| `m_Color` | `light_color` | Direct RGB |
| `m_Intensity` | `light_energy` | Direct float (may need scaling factor) |
| `m_Range` | `omni_range` / `spot_range` | Direct float |
| `m_SpotAngle` | `spot_angle` | `godot_angle = unity_angle / 2.0` |
| `m_Shadows.m_Type` | `shadow_enabled` | 0 = no shadows, 1/2 = shadows on |

### Camera Conversion Details

| Unity Property | Godot Property | Conversion |
|---|---|---|
| `field of view` | `fov` | Direct (both vertical FOV in degrees) |
| `near clip plane` | `near` | Direct float |
| `far clip plane` | `far` | Direct float |
| `orthographic` | `projection` | 0 = perspective, 1 = orthogonal |
| `orthographic size` | `size` | Direct float |

### .tscn Output Format

```ini
[gd_scene load_steps=3 format=3]

[ext_resource type="PackedScene" path="res://Models/building.fbx" id="1"]
[ext_resource type="Material" path="res://Materials/brick.tres" id="2"]

[node name="MainLevel" type="Node3D"]

[node name="Building" parent="." instance=ExtResource("1")]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 5, 0, -3)

[node name="GroupNode" type="Node3D" parent="."]

[node name="Sun" type="DirectionalLight3D" parent="."]
light_color = Color(1, 0.95, 0.85, 1)
light_energy = 1.5
shadow_enabled = true
```

---

## 8. Prefab Conversion

### Approach: Prefabs → Godot Scenes (.tscn)

Each Unity `.prefab` file is converted to its own `.tscn` file. Scenes that instance prefabs use `instance = ExtResource(...)` to reference the prefab's `.tscn`.

### Prefab Processing

1. Parse the `.prefab` file (same YAML format as scenes)
2. Build the internal hierarchy (same as scene conversion)
3. Output as a standalone `.tscn` file in the same relative path

### Prefab Instancing in Scenes

When a scene contains a `PrefabInstance` component (classID 1001):

1. Resolve the prefab GUID to find the corresponding `.tscn`
2. Create an instanced node: `instance=ExtResource("<id>")`
3. Apply overrides (see below)

### Override Support (V1 Scope)

V1 supports the two most common override types from `m_Modifications`:

**Transform overrides:**
- `m_LocalPosition`, `m_LocalRotation`, `m_LocalScale` on any target within the prefab
- Applied by setting the `transform` property on the instanced node or its children

**Material overrides:**
- `m_Materials.Array.data[N]` changes on MeshRenderer targets
- Applied by setting `surface_material_override/N` on the relevant mesh node

All other override types are **logged as warnings** and skipped.

### Nested Prefabs

- **One level deep:** If a prefab references another prefab, the inner prefab reference is **flattened** (its hierarchy is baked directly into the outer prefab's .tscn)
- Deeper nesting is detected and logged as a warning
- This avoids recursive override resolution while handling the majority of real-world cases

---

## 9. Godot Project Generation

### Output Structure

```
output_folder/
├── project.godot
├── Scenes/
│   └── MainLevel.tscn
├── Models/
│   └── building.fbx
├── Textures/
│   ├── brick_albedo.png
│   └── brick_normal.png
├── Materials/
│   └── brick.tres
└── Prefabs/
    └── Lamp.tscn
```

### Folder Mapping

Unity paths are mapped to Godot paths by stripping the `Assets/` prefix:

```
Assets/Models/building.fbx  →  Models/building.fbx
Assets/Textures/brick.png   →  Textures/brick.png
Assets/Scenes/Main.unity    →  Scenes/Main.tscn
```

### Special Characters in Names

Unity allows spaces, parentheses, unicode, and other special characters in file and folder names. These are preserved as-is in the output — Godot supports them. When writing references in `.tscn`/`.tres` files, paths containing special characters must be quoted with double quotes (e.g., `path="res://Models/Old House (1)/building.fbx"`).

### project.godot

A minimal but valid `project.godot` file:

```ini
; Engine configuration file.
; It's best edited using the editor, so don't edit it unless you know what you're doing.

config_version=5

[application]
config/name="<derived from package name>"
config/features=PackedStringArray("4.6")

[rendering]
renderer/rendering_method="forward_plus"
```

---

## 10. GUI

### Framework

- **Dear ImGui** with **GLFW + OpenGL3** backend
- Native file/folder dialogs via **nativefiledialog-extended**

### Layout: Single-Screen Wizard

```
+----------------------------------------------------+
| Unity2Godot Converter                         [v1]  |
+----------------------------------------------------+
|                                                     |
| Package:  [________________________] [Browse...]    |
| Output:   [________________________] [Browse...]    |
|                                                     |
|  [ Convert ]           Progress: [========>  ] 73%  |
|                                                     |
| Log:                                                |
| +--------------------------------------------------+|
| | [INFO]  Extracting package...                    ||
| | [INFO]  Found 142 assets (34 tex, 12 fbx, ...)  ||
| | [INFO]  Building GUID table...                   ||
| | [INFO]  Converting textures... (34/34)           ||
| | [INFO]  Transcoded brick.psd → brick.png         ||
| | [WARN]  Unknown shader "Custom/Water" on         ||
| |         material WaterSurface.mat → default mat  ||
| | [INFO]  Converting scene: MainLevel.unity        ||
| | [INFO]  Done! 3 warnings, 0 errors               ||
| +--------------------------------------------------+|
|                                                     |
| Skip Report:                                        |
| +--------------------------------------------------+|
| | 4 C# scripts skipped                            ||
| | 2 particle systems skipped                       ||
| | 1 animator controller skipped                    ||
| | 1 custom shader skipped (Custom/Water.shader)    ||
| +--------------------------------------------------+|
+----------------------------------------------------+
```

### UI Elements

- **Package path:** Text input + Browse button (opens native file dialog, filter: `*.unitypackage`)
- **Output path:** Text input + Browse button (opens native folder dialog)
- **Convert button:** Disabled until both paths are set. Disabled during conversion. Text changes to "Cancel" during conversion.
- **Progress bar:** Shows overall conversion progress with current phase label
- **Log window:** Scrolling text area with `[INFO]`, `[WARN]`, `[ERROR]` prefixed lines. Auto-scrolls to bottom. Color-coded (white/yellow/red).
- **Skip report:** Appears after conversion. Categorized summary of all skipped/unsupported assets with counts and file paths.

---

## 11. Error Handling

### Philosophy: Best-Effort with Warnings

The converter **never aborts** due to a single bad asset. Every asset is processed independently.

### Error Categories

| Severity | Behavior | Examples |
|---|---|---|
| **INFO** | Normal progress | "Converting texture brick.png", "Found 142 assets" |
| **WARN** | Asset partially converted or skipped | Unknown shader, missing texture reference, unsupported override type, nested prefab depth > 1 |
| **ERROR** | Asset failed to convert entirely | Corrupt FBX file, unparseable scene YAML, I/O failure on a specific file |
| **FATAL** | Conversion cannot continue | Invalid/corrupt .unitypackage, output directory not writable, out of disk space |

### Missing References

When a GUID reference cannot be resolved:

- **Texture ref in material:** Skip the texture, use default value, log warning
- **Material ref in scene:** Use default material, log warning
- **Mesh/FBX ref in scene:** Create an empty Node3D placeholder, log warning
- **Prefab ref in scene:** Create an empty Node3D placeholder, log warning

### Skip Report

After conversion completes, a categorized summary is produced:

```
=== Skip Report ===
Skipped asset types (not supported in V1):
  C# Scripts:           4 files
  Particle Systems:     2 files
  Animator Controllers: 1 file
  Custom Shaders:       1 file
  Audio Clips:          3 files
  
  Details:
    Scripts/PlayerController.cs
    Scripts/GameManager.cs
    ...
```

---

## 12. Project Structure

```
unity2godot/
├── CMakeLists.txt
├── SPEC.md
├── README.md
├── src/
│   ├── main.cpp                    # Entry point, ImGui setup, main loop
│   ├── gui/
│   │   ├── app_window.h/.cpp       # ImGui window management, GLFW setup
│   │   └── converter_ui.h/.cpp     # Converter UI layout and state
│   ├── converter/
│   │   ├── converter.h/.cpp        # Main conversion orchestrator
│   │   ├── package_extractor.h/.cpp # .unitypackage tar.gz extraction (miniz + custom tar reader)
│   │   ├── guid_table.h/.cpp       # GUID → path resolution table
│   │   ├── unity_yaml_parser.h/.cpp # Custom Unity YAML parser
│   │   ├── texture_converter.h/.cpp # Texture copy/transcode + .import generation
│   │   ├── material_converter.h/.cpp# Unity material → .tres conversion
│   │   ├── scene_converter.h/.cpp  # Unity scene → .tscn conversion
│   │   ├── prefab_converter.h/.cpp # Unity prefab → .tscn conversion
│   │   ├── light_converter.h/.cpp  # Unity light → Godot light node
│   │   ├── camera_converter.h/.cpp # Unity camera → Godot camera node
│   │   ├── project_writer.h/.cpp   # Godot project.godot + folder structure
│   │   └── coord_convert.h         # Unity ↔ Godot coordinate conversion
│   └── util/
│       ├── log.h/.cpp              # Logging system (INFO/WARN/ERROR)
│       └── types.h                 # Common types, AssetEntry, etc.
└── thirdparty/
    ├── ufbx/                       # FBX parser
    ├── imgui/                      # Dear ImGui
    ├── glfw/                       # GLFW windowing
    ├── miniz/                      # gzip/deflate
    └── nfd/                        # nativefiledialog-extended
```

---

## 13. Out of Scope (V1)

The following are explicitly **not supported** in V1 and will be logged in the skip report:

- C# scripts and MonoBehaviour components
- Custom shaders (beyond URP Lit/Unlit and legacy Standard)
- Skinned meshes / SkinnedMeshRenderer
- Animations / Animator / AnimationClip
- Audio clips and AudioSource
- Particle systems
- UI Canvas / UGUI elements
- Terrain data
- NavMesh data
- Physics materials (beyond what colliders use)
- Asset Bundles
- ScriptableObjects
- Lightmap data (baked lighting)
- Reflection probes
- Video clips
- Sprite / 2D assets
- Nested prefabs deeper than 1 level
- Prefab variants

---

## 14. Known Limitations & Risks

1. **FBX instancing granularity:** Since we instance entire FBX files rather than individual meshes, a Unity scene that uses 3 different meshes from the same FBX will create 3 instances of the full model. This may produce visual duplicates if the FBX contains multiple objects. Mitigation: log a warning when this is detected.

2. **Material overrides on FBX instances:** The converter uses ufbx to extract node names from FBX files and constructs override paths assuming Godot's importer produces matching names. This works in ~80-90% of cases since Godot also uses ufbx internally. It can fail if Godot renames nodes (duplicate suffixes, sanitization) or restructures the hierarchy. Mitigation: best-effort path matching, warning on failure. Failed overrides result in the FBX's embedded materials being used instead.

3. **Unity YAML edge cases:** Stripped/binary scenes won't parse. The converter requires text-mode `.unity` files (which is what `.unitypackage` always contains). Multi-scene setups (additive loading) are converted as independent scenes.

4. **Smoothness → Roughness inversion:** Unity stores smoothness in the alpha channel of the metallic map texture. Godot uses a separate roughness value/texture. When a metallic-smoothness packed texture is detected, the converter would ideally split/invert the alpha channel. V1 will do the scalar inversion (`roughness = 1 - smoothness`) but will **not** modify texture data. Packed metallic-smoothness textures will produce a warning.

5. **Light intensity:** Unity and Godot use different light intensity units. Unity URP uses physical units (lumens/lux) while Godot uses an arbitrary energy multiplier. Direct value copy may produce too-bright or too-dim lighting. Mitigation: copy value as-is, document that manual adjustment may be needed.

6. **Unsupported texture formats:** PSD and EXR files are copied as-is but Godot cannot import them. The user must manually convert these to PNG/JPG. Automatic transcoding is planned for a future version.

7. **FBX import-time root transforms:** Unity and Godot may apply different corrections when importing the same FBX file (e.g., axis rotation, scale factor), since FBX files can be authored in various coordinate systems (Z-up, Y-up) and unit scales. This can cause models to appear rotated or scaled differently compared to Unity, even though our scene-level coordinate conversion is correct. The converter cannot control Godot's FBX import behavior. Mitigation: the user manually adjusts affected models in Godot.

---

## 15. Future Versions (Not In Scope — Reference Only)

- V1.x: Texture transcoding (PSD/EXR → PNG) via stb_image
- V1.x: Texture import settings mapping (normal map detection, filter/wrap modes, sRGB) via .import files
- V2: Skinned meshes, animations, blend shapes
- V3: Audio import, particle system basic conversion
- V4: C# → GDScript transpilation (limited subset)
- V5: Custom shader → Godot shader conversion
