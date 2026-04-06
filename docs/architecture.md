# Architecture

## Scope

The supported runtime is intentionally narrow: one EEex-loaded Windows DLL, one validated BGEE build manifest, and one feature set centered on tile upscaling. The code is structured so additional game/build support is a data expansion exercise, not another rewrite.

There is also an experimental sprite-shader workspace in `shaders/sprite/`. That path is intentionally separate from the supported DLL pipeline. It is for live GLSL replacement experiments against the target game install, not a new runtime subsystem inside the DLL.

## Runtime Modules

`src/iee/game/build_manifest.*`

- Declares build-specific patterns, fallback RVAs, runtime offsets, and verified render callsites.
- The current manifest is BGEE `2.6.6.x`.

`src/iee/game/game_addrs.*`

- Resolves `CInfGame::LoadArea` and `CVidTile::RenderTexture`.
- Uses manifest patterns first, then configured or manifest RVAs as fallback.

`src/iee/game/renderer.*`

- Resolves draw API entry points from `CVidTile::RenderTexture`.
- Handles opcode-aware rel32 decoding, including the `DrawPopState` tail jump.
- Applies GL texture parameter upgrades.

`src/iee/game/tis_runtime.*`

- Provides explicit runtime views for `CRes`, `CResTile`, `CResTileSet`, `CResPVR`, and the authored TIS header.
- Reads build-specific offsets from the manifest instead of hardcoding them throughout hooks.

`src/iee/game/tile_upscale.*`

- Encapsulates scale detection.
- TIS header metadata is authoritative.
- UV / texture-id heuristics remain fallback only.

`src/iee/hooks.*`

- Owns the runtime hook lifecycle.
- `LoadArea` resets area-scoped detection state.
- `RenderTexture` applies the tile upscale path once scale is known.

`shaders/sprite/runtime_baseline/*`

- Stores captured live `fpsprite.glsl` and `fpselect.glsl` sources from the target runtime.
- Serves as the immutable reference point for sprite-only experiments.

`shaders/sprite/v1/*`

- Holds the repo-owned working copies for sprite-only upscale experiments.
- Current design target is a one-pass shader replacement that preserves vanilla sprite behavior and does not require new DLL hooks by default.

## Build Layout

The CMake graph is split on purpose:

- `iee_common` is host-safe and can be compiled in WSL for tests.
- `InfinityEngine-Enhancer` is Windows-only and links MinHook, `psapi`, and `opengl32`.
- `release_bundle` packages the DLL, EEex loader script, and sample INI into one directory.

This keeps reverse-engineering and parsing code testable without requiring a Windows toolchain for every edit.

## Operational Boundaries

- Use WSL for code analysis, docs, and host tests.
- Use Windows or CI for final DLL validation.
- Unknown or incompatible builds should fail during manifest/address/callsite resolution instead of reaching gameplay with bad hooks.
- For sprite-shader work, use live shader files and runtime screenshots as the primary validation loop.
- Do not conflate the tile DLL path with the sprite shader-file path. `CVidTile::RenderTexture` remains the terrain/tile hook, not the entrypoint for sprite v1.

## Archival Code

[shaders/InfinityEngine-Enhancer.cpp](../shaders/InfinityEngine-Enhancer.cpp) is preserved as research material only. It is not part of the supported build and should not be treated as the source of truth for current runtime behavior.
