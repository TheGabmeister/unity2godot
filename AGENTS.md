# AGENTS.md

## Purpose

This repository builds a cross-platform desktop tool that converts Unity `.unitypackage` archives into Godot 4.6.1 projects.

The codebase is still early-stage. Keep changes small, practical, and aligned with `SPEC.md`.

## Source Of Truth

- Product scope and conversion behavior: `SPEC.md`
- Current build wiring and dependencies: `CMakeLists.txt`
- High-level project notes: `README.md`

If `SPEC.md` and the current code disagree, treat `SPEC.md` as the intended v1 target unless the user says otherwise.

## Current State

- The repo already uses a modular source layout under `src/gui/`, `src/converter/`, and `src/util/`.
- `src/main.cpp` is only the bootstrap and app entry point.
- The main UI lives in `src/gui/app_window.*` and `src/gui/converter_ui.*`.
- The conversion pipeline is orchestrated from `src/converter/converter.*`.
- Scene and prefab conversion share core logic in `src/converter/scene_converter.*`.
- Third-party libraries are vendored under `thirdparty/`.
- `test/` contains sample inputs and manual verification assets, including `test/test.unitypackage`, `test/URP/`, and a generated sample output under `test/godot/`.

When adding new code, prefer extending the existing module structure instead of growing `main.cpp` or creating catch-all utility files.

## Architecture Snapshot

The app has two main layers:

- A Dear ImGui GUI running on the main thread
- A conversion pipeline running on a background worker thread

Current pipeline order:

1. Extract `.unitypackage` data to a temp directory
2. Build the GUID table from `pathname` and `asset.meta`
3. Copy textures and FBX files into the output project
4. Convert Unity materials to Godot `.tres`
5. Convert Unity prefabs to Godot `.tscn`
6. Convert Unity scenes to Godot `.tscn`
7. Generate `project.godot`

Important implementation notes:

- `Converter::run()` is the main orchestrator.
- Progress, cancellation, and completion state are shared between threads; keep any new cross-thread state explicit and simple.
- Scene and prefab conversion share `buildTscnData()` and `writeTscnFile()` in `scene_converter.cpp`.
- Coordinate handedness conversion is applied to scene/prefab transforms, not by rewriting FBX contents.
- FBX files are copied as-is and left for Godot to import.

## Build And Run

Configure:

```powershell
cmake -S . -B build
```

Build:

```powershell
cmake --build build
```

Run from the build output directory or via the generated IDE project.

Typical executable locations:

- Windows multi-config generators: `build/Debug/unity2godot.exe`
- Single-config generators on Windows/Linux/macOS: `build/unity2godot`

If you add new source files or dependencies, update `CMakeLists.txt` in the same change.

## Platform Expectations

- Target platforms: Windows, macOS, Linux
- Language: C++17
- Build system: CMake 3.16+
- GUI stack: Dear ImGui + GLFW + OpenGL3
- Native dialogs: `nativefiledialog-extended`

Do not introduce platform-specific behavior unless the spec requires it or it is guarded cleanly.

## Dependencies

All current third-party code is vendored. The main libraries in active use are:

- `ufbx`
- Dear ImGui
- GLFW
- miniz
- `nativefiledialog-extended`

Prefer wrapping third-party APIs in local adapter code rather than scattering direct usage across the codebase.

## V1 Guardrails

These are easy to accidentally violate. Keep them explicit in code reviews and implementations:

- V1 copies textures as-is. Do not add PSD/EXR transcoding unless requested.
- V1 does not generate Godot `.import` files. Godot generates them on first open.
- V1 copies FBX files as-is and lets Godot import them.
- V1 may use `ufbx` only for FBX inspection, name extraction, fileID resolution, and validation.
- V1 supports static meshes, textures, materials, prefabs, and scenes only.
- V1 does not support scripts, animation, skinned meshes, audio, terrain, UI, or deep nested prefabs.
- Best-effort conversion is preferred: log warnings and continue unless the failure is truly fatal.

## Codebase Conventions

### Unity YAML parsing

The custom parser handles Unity's YAML-like format, not general YAML. Be careful around:

- Multi-document files using `--- !u!<classID> &<fileID>`
- Same-indent sequences under keys such as `m_Component`
- Flow mappings like `{fileID: 123}`
- Sequence items that are single-key maps, such as material property arrays
- Prefab override property paths like `m_LocalPosition.x`

If you change parser behavior, keep it narrowly targeted to Unity's format and avoid turning it into a generic YAML layer.

### Unity class IDs that matter

These class IDs show up repeatedly in scene and prefab work:

- `1`: `GameObject`
- `4`: `Transform`
- `20`: `Camera`
- `21`: `Material`
- `23`: `MeshRenderer`
- `25`: `Renderer`
- `33`: `MeshFilter`
- `108`: `Light`
- `1001`: `PrefabInstance`
- `1660057539`: `SceneRoots`

### FBX handling

- FBX assets are preserved and referenced as Godot external scenes.
- `ufbx` is used only to inspect names and resolve fileID relationships.
- Unity can treat an FBX as an implicit prefab. Scene conversion should check prefab mappings first, then fall back to GUID entries for FBX-backed instances.
- When multiple Unity sub-object references point into the same FBX, prefer warning and continuing over trying to add out-of-scope mesh extraction behavior.

### Paths and output layout

- Preserve folder structure by stripping the Unity `Assets/` prefix.
- Normalize incoming Unity paths carefully.
- Be careful with quoting and escaping in generated `.tscn` and `.tres` files.
- Keep output paths valid across Windows, macOS, and Linux.

## Implementation Guidance

- Prefer small, focused modules over large utility blobs.
- Keep parsing, conversion, UI, and filesystem concerns separated.
- Avoid hidden global state. Pass explicit context objects where practical.
- Favor plain standard-library code unless a vendored dependency clearly simplifies the task.
- Keep user-visible logs actionable and specific.
- If behavior is ambiguous, choose the simpler v1-compatible option and document the assumption in code or the final note.

## Safety-Critical Areas

Take extra care in these parts of the converter:

- Archive extraction: reject path traversal and malformed tar entries.
- Generated `.tscn` and `.tres` text: ensure quoting and escaping are valid.
- GUID and fileID resolution: missing references should degrade gracefully.
- Scene transform conversion: keep behavior consistent with the spec and document assumptions in code.
- FBX override paths: treat them as best-effort and warn on mismatch.
- Cross-thread UI/converter communication: avoid races and hidden ownership.

## Logging And UX

- User-visible logging matters for this tool.
- Prefer actionable warnings over vague failures.
- When skipping unsupported assets, include enough context for the user to fix things manually in Godot.
- Avoid noisy logs for normal, expected cases unless the spec explicitly wants them surfaced.

## Testing Expectations

There is no full automated test suite or CTest target yet. When making converter changes:

- Build the project successfully.
- Sanity-check new code paths locally when possible.
- Prefer using `test/test.unitypackage` for manual conversion checks.
- Treat `test/godot/` and other files under `test/` as fixtures or manual artifacts. Do not refresh or rewrite them unless the task explicitly calls for updating those outputs.
- If you cannot verify behavior end-to-end, say so clearly in your final note.

If you add helpers that are easy to test and the repo later gains a test target, add lightweight tests close to that logic.

## Third-Party Code

- Do not modify vendored libraries unless the task explicitly requires it.
- Prefer local wrappers or adapter code over spreading third-party calls throughout the codebase.
- If a vendored library must be patched, keep the patch minimal and document why.

## Repository Hygiene

- Keep diffs tight.
- Avoid incidental churn in `test/`, generated sample outputs, or editor state files.
- Before editing or regenerating anything under `test/`, confirm it is necessary for the task.
- Do not silently clean up, reformat, rename, or reorganize large fixture directories as part of unrelated work.

## File Organization

When creating new source files, prefer the structure planned in `SPEC.md`:

- `src/gui/` for ImGui windowing and UI state
- `src/converter/` for extraction, parsing, conversion, and project writing
- `src/util/` for shared types and logging

## Agent Workflow

- Read the relevant section of `SPEC.md` before changing conversion behavior.
- Check the current implementation before assuming a module is missing.
- Keep diffs coherent: implementation, build wiring, and any necessary spec or doc updates should land together.
- Do not silently change v1 scope. If the task pushes beyond spec, pause and call it out.
- Prefer best-effort conversion with warnings over hard failure for isolated bad assets.
