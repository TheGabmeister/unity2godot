# AGENTS.md

## Purpose

This repository builds a desktop tool that converts Unity `.unitypackage` archives into Godot 4.6.1 projects.

The codebase is early-stage. Keep changes small, practical, and aligned with `SPEC.md`.

## Source Of Truth

- Product and conversion behavior: `SPEC.md`
- Current build setup and dependencies: `CMakeLists.txt`
- High-level project notes: `README.md`

If `SPEC.md` and the current code disagree, treat `SPEC.md` as the intended v1 target unless the user says otherwise.

## Current State

- The repo currently contains a minimal executable in `src/main.cpp`.
- Third-party libraries are vendored under `thirdparty/`.
- The long-term module layout described in `SPEC.md` does not fully exist yet.

When adding new code, prefer creating the structure described in `SPEC.md` instead of growing `main.cpp`.

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

If you add new dependencies or source files, update `CMakeLists.txt` in the same change.

## Platform Expectations

- Target platforms: Windows, macOS, Linux
- Language: C++17
- Build system: CMake 3.16+
- GUI stack: Dear ImGui + GLFW + OpenGL3
- Native dialogs: `nativefiledialog-extended`

Do not introduce platform-specific behavior unless the spec requires it or it is guarded cleanly.

## V1 Guardrails

These are easy to accidentally violate. Keep them explicit in code reviews and implementations:

- V1 copies textures as-is. Do not add PSD/EXR transcoding unless requested.
- V1 does not generate Godot `.import` files. Godot generates them on first open.
- V1 copies FBX files as-is and lets Godot import them.
- V1 may use `ufbx` only for FBX inspection, name extraction, fileID resolution, and validation.
- V1 supports static meshes, textures, materials, prefabs, and scenes only.
- V1 does not support scripts, animation, skinned meshes, audio, terrain, UI, or deep nested prefabs.
- Best-effort conversion is preferred: log warnings and continue unless the failure is truly fatal.

## Implementation Guidance

- Prefer small, focused modules over large utility blobs.
- Keep parsing, conversion, UI, and filesystem concerns separated.
- Avoid hidden global state. Pass explicit context objects where practical.
- Favor plain standard-library code unless a vendored dependency clearly simplifies the task.
- Preserve folder structure by stripping the Unity `Assets/` prefix as described in `SPEC.md`.
- Be careful with path normalization, quoting, and cross-platform filesystem behavior.

## Safety-Critical Areas

Take extra care in these parts of the converter:

- Archive extraction: reject path traversal and malformed tar entries.
- Generated `.tscn` and `.tres` text: ensure quoting and escaping are valid.
- GUID and fileID resolution: missing references should degrade gracefully.
- Scene transform conversion: keep behavior consistent with the spec and document assumptions in code.
- FBX override paths: treat them as best-effort and warn on mismatch.

## Logging And UX

- User-visible logging matters for this tool.
- Prefer actionable warnings over vague failures.
- When skipping unsupported assets, include enough context for the user to fix things in Godot manually.
- Avoid noisy logs for normal, expected cases unless the spec explicitly wants them surfaced.

## Testing Expectations

There is no full automated test suite yet. When making converter changes:

- Build the project successfully.
- Sanity-check new code paths locally when possible.
- If you cannot verify behavior end-to-end, say so clearly in your final note.

If you add parser or conversion helpers that are easy to unit test, add lightweight tests when the repo gains a test target.

## Third-Party Code

- Do not modify vendored libraries unless the task explicitly requires it.
- Prefer wrapping third-party APIs in local adapter code instead of scattering direct calls everywhere.
- If a vendored library must be patched, keep the patch minimal and document why.

## File Organization

When creating new source files, prefer the structure planned in `SPEC.md`:

- `src/gui/` for ImGui windowing and UI state
- `src/converter/` for extraction, parsing, conversion, and project writing
- `src/util/` for shared types and logging

## Agent Workflow

- Read the relevant section of `SPEC.md` before changing conversion behavior.
- Keep diffs coherent: implementation, build wiring, and any necessary spec/docs updates should land together.
- Do not silently change v1 scope. If the task pushes beyond spec, pause and call it out.
- If behavior is ambiguous, choose the simpler v1-compatible option and document the assumption.
