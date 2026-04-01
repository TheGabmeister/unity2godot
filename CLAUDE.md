# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Unity2Godot — a C++17 cross-platform desktop tool that converts `.unitypackage` files into Godot 4.6.1 projects. See `SPEC.md` for the full technical specification and `AGENTS.md` for development guidelines and V1 guardrails.

## Build

```bash
cmake -B build
cmake --build build
./build/Debug/unity2godot.exe   # Windows
./build/unity2godot             # Linux/macOS
```

No test framework or linter is configured. Use `test/test.unitypackage` for manual conversion checks; open the output in Godot to verify.

## Architecture

Two layers: a Dear ImGui GUI (GLFW + OpenGL3) on the main thread, and a conversion pipeline on a background worker thread (`std::thread`). Thread-safe communication via atomic flags (`running_`, `done_`, `cancelled_`) and mutex-protected progress/skip-report structs.

**Conversion pipeline** (in order):
1. Extract `.unitypackage` (tar.gz) to temp dir using miniz + custom tar reader
2. Build `GuidTable` (GUID → `AssetEntry`) from extracted `pathname` and `asset.meta` files
3. Copy textures and FBX files to output (FBX parsed with ufbx for node name extraction only)
4. Convert Unity materials (`.mat` YAML) → Godot `.tres` — returns `MaterialMap` (guid → `res://` path)
5. Convert Unity prefabs (`.prefab` YAML) → Godot `.tscn` — returns `PrefabMap` (guid → `res://` path)
6. Convert Unity scenes (`.unity` YAML) → Godot `.tscn` — consumes MaterialMap + PrefabMap
7. Generate `project.godot`

Each step checks a cancellation flag and reports progress. The orchestrator is `Converter::run()` in `converter.cpp`.

**Scene/prefab conversion** shares core logic: `buildTscnData()` in `scene_converter.cpp` parses Unity YAML, builds a `GodotNode` tree, and resolves references. `writeTscnFile()` serializes to `.tscn`. The prefab converter calls these same functions.

**Coordinate system:** Unity is left-handed (Y-up, Z-forward), Godot is right-handed (Y-up, -Z-forward). Handedness conversion is applied at scene-level transforms only (see `coord_convert.h`); Godot's FBX importer handles FBX coordinate conversion internally.

## Dependencies

All vendored in `thirdparty/`. Libraries have been stripped to essentials (no tests/examples/docs).

| Library | CMake target | Notes |
|---|---|---|
| ufbx 0.20.0 | `ufbx` | Single-file C library, built as static lib |
| Dear ImGui 1.92.6 | `imgui` | Built manually (no upstream CMakeLists.txt), only GLFW+OpenGL3 backends kept |
| GLFW 3.4 | `glfw` | Added via `add_subdirectory`, examples/tests/docs disabled |
| miniz 3.1.1 | `miniz` | Added via `add_subdirectory` |
| NFD Extended 1.3.0 | `nfd` | In `thirdparty/nfd-extended/`, added via `add_subdirectory` |

OpenGL is linked as a system library (platform-specific in CMakeLists.txt).

## Key Conventions

- Unity YAML class IDs that matter: 1=GameObject, 4=Transform, 21=Material, 23=MeshRenderer, 33=MeshFilter, 108=Light, 20=Camera, 1001=PrefabInstance, 1660057539=SceneRoots (Unity 2022+)
- FBX files are kept as-is for Godot to import; ufbx is used only to extract node/mesh names for material override paths
- Godot `.tscn`/`.tres` files are generated as direct text output, not via Godot's CLI
- Unity asset references use `{fileID, guid, type}` tuples — resolved via the GUID table built during extraction
- Unity folder structure is preserved by stripping the `Assets/` prefix (e.g., `Assets/Models/foo.fbx` → `Models/foo.fbx`)

## Unity YAML Parser Quirks

The custom parser (`unity_yaml_parser.cpp`) handles Unity's non-standard YAML variant. Key patterns to know:

- **Same-indent sequences:** Unity puts sequence items at the same indent as the key, not deeper. `m_Component:` followed by `- component: {fileID: X}` at the same indent level. The parser handles this as a special case.
- **Flow mappings in sequences:** `- {fileID: 123, guid: abc, type: 3}` is a sequence item containing a flow mapping.
- **Sequence-of-single-key-maps:** Material properties like `m_TexEnvs` and `m_Floats` are sequences where each item is a map with one key (e.g., `- _BaseMap: ...`). Iterate the sequence to look up named entries.
- **PrefabInstance modifications:** Property overrides use dot-path strings like `m_LocalPosition.x` with separate entries per component, which must be reassembled into full Vec3/Quat values.

## FBX as Implicit Prefab

In Unity, FBX files act as implicit prefabs — `PrefabInstance` components can reference an FBX file's GUID directly (not just `.prefab` GUIDs). The scene converter checks the prefab map first, then falls back to the GUID table to find FBX files and instance them as `PackedScene` resources.
