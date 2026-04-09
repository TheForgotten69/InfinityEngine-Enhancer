# AGENTS

## Audience And Constraints

- Write for a principal or senior engineer.
- Prefer KISS, YAGNI, DRY, and SOLID pragmatically.
- Do not add dependencies or architectural layers unless they pay for themselves immediately.

## Environment

- Use WSL for analysis, docs, and host-side tests.
- Use Windows or CI for the final DLL build and runtime validation.
- The supported runtime target today is BGEE `2.6.6.x`.

## Build And Test Commands

- WSL host tests:
  `cmake -S . -B cmake-build-debug -DBUILD_TESTING=ON`
  `cmake --build cmake-build-debug --target iee_tests`
  `ctest --test-dir cmake-build-debug --output-on-failure`
- Windows DLL build:
  `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DIEE_BUILD_WINDOWS_DLL=ON -DBUILD_TESTING=ON`
  `cmake --build build --config Release --target release_bundle`

## Runtime Facts

- Hook `CVidTile::RenderTexture`, not `CVidCell::RenderTexture`.
- `LoadArea` is the area boundary and resets scale detection.
- TIS header `+0x14` is the authoritative tile-dimension field.
- `0x40` means standard tiles.
- `0x100` means authored 4x tiles.
- `CResTileSet::h` is optional. Standard tilesets can have `header == null` while still exposing a valid 12-byte PVR entry table through `pData`.
- Deterministic detection order for this build is:
  `TIS header -> PVR entry table smallest positive step -> legacy heuristic fallback`
- `+0x1DC` is only the current linear-tiles tone flag for this build.
- If you need live runtime facts, add DLL logging. Ghidra is useful for offline layout review but cannot answer live process-state questions on its own.

## Repo Layout

- `src/iee/game/build_manifest.*` holds build-specific offsets, patterns, and callsites.
- `src/iee/game/tis_runtime.*` holds explicit runtime views.
- `src/iee/game/tile_upscale.*` holds scale selection logic.
- `docs/` contains the architecture and reverse-engineering notes future agents should read first.
- `shaders/InfinityEngine-Enhancer.cpp` is archival research code only.

## Done Means

- Critical paths compile.
- Host-side tests pass.
- Windows-only build changes are guarded so WSL does not wander into MinHook or `windows.h` failures.
- Trade-offs and remaining Windows-only validation gaps are called out explicitly.
- Do not claim generalized build detection yet. The manifest layer is data-driven, but manifest selection is still effectively pinned to the currently validated BGEE build until explicit version/build probing is added.
