# Architecture

## Scope

The supported runtime is intentionally narrow: one EEex-loaded Windows DLL, one validated BGEE build manifest, and one feature set centered on tile upscaling. The code is structured so additional game/build support is a data expansion exercise, not another rewrite.

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

`src/iee/game/runtime_types_x64.h`

- Holds the curated x64 type surface used for rapid reverse-engineering around tiles, sprites, and renderer-adjacent engine state.
- Small PODs are modeled directly; larger engine objects keep exact offsets for selected named fields and leave the rest opaque.

`src/iee/game/file_formats.h`

- Holds exact on-disk / loaded resource layout definitions mirrored from EEex docs for `TIS`, `WED`, `BAM`, and `PVRZ`-adjacent structures.

`src/iee/game/eeex_doc_layouts_x64.h`

- Holds exact EEex field maps for large runtime classes where mirroring every nested type as hand-written C++ would be brittle and low signal.
- This keeps the full documented field surface for `CInfGame`, `CInfinity`, `CGameSprite`, and shader-adjacent types in-repo.

`src/iee/game/tile_upscale.*`

- Encapsulates scale detection.
- TIS header metadata is authoritative.
- UV / texture-id heuristics remain fallback only.

`src/iee/hooks.*`

- Owns the runtime hook lifecycle.
- `LoadArea` resets area-scoped detection state.
- `RenderTexture` applies the tile upscale path once scale is known.

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

## Archival Code

[shaders/InfinityEngine-Enhancer.cpp](../shaders/InfinityEngine-Enhancer.cpp) is preserved as research material only. It is not part of the supported build and should not be treated as the source of truth for current runtime behavior.
