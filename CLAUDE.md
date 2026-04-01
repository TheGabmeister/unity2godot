# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Unity2Godot — a C++17 cross-platform desktop tool that converts `.unitypackage` files into Godot 4.6.1 projects. See `SPEC.md` for the full technical specification.

## Build

```bash
cmake -B build
cmake --build build
./build/Debug/unity2godot.exe   # Windows
./build/unity2godot             # Linux/macOS
```

No test framework is set up yet. No linter is configured.

## Architecture

The app has two main layers: a Dear ImGui GUI (GLFW + OpenGL3) and a conversion pipeline that runs on a background thread.

**Conversion pipeline** (in order):
1. Extract `.unitypackage` (tar.gz) to temp dir using miniz + custom tar reader
2. Build GUID→path table from extracted `pathname` and `asset.meta` files
3. Copy textures and FBX files to output (FBX files are parsed with ufbx for node name extraction, not converted)
4. Convert Unity materials (`.mat` YAML) → Godot `.tres` files
5. Convert Unity prefabs (`.prefab` YAML) → Godot `.tscn` files
6. Convert Unity scenes (`.unity` YAML) → Godot `.tscn` files
7. Generate `project.godot`

Unity scene/prefab/material files use a non-standard YAML variant with `!u!` tags — parsed by a custom lightweight parser, not a YAML library.

**Coordinate system:** Unity is left-handed (Y-up, Z-forward), Godot is right-handed (Y-up, -Z-forward). Handedness conversion is applied at scene-level transforms only; Godot's FBX importer handles FBX coordinate conversion internally.

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

- Unity YAML class IDs that matter: 1=GameObject, 4=Transform, 21=Material, 23=MeshRenderer, 33=MeshFilter, 108=Light, 20=Camera, 1001=PrefabInstance
- FBX files are kept as-is for Godot to import; ufbx is used only to extract node/mesh names for material override paths
- Godot `.tscn`/`.tres` files are generated as direct text output, not via Godot's CLI
- Unity asset references use `{fileID, guid, type}` tuples — resolved via the GUID table built during extraction
