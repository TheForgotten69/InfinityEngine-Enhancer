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
- The renderer is an OpenGL 4.6 **compatibility profile** context (legacy `wglCreateContext`; modern GL loads via `wglGetProcAddress`).
- The engine compiles nine named GLSL programs (`fpSEAM`, `fpSprite`, `fpSELECT`, `fpCatRom`, ...). The verified slot table and shader-replacement strategy live in the graphics roadmap spec (see Docs below).
- The engine does **zero** lighting work. Day/night is a swap of authored assets. `CGameArea` holds authored per-area bitmaps (`m_bmLum` +0x260, `m_pbmLumNight` +0x380, `m_bmHeight` +0x388) usable as static lookup data only.
- **Never detour `CVisibilityMap::BltFogOWar3d`** — crashes on any modification (stack canary, confirmed experimentally). `CInfinity::RenderFog` is setup-only but is safe to hook as a bracket around the fog pass.
- Prefer `SDL_GL_SwapWindow` (SDL2.dll export) over `DrawFlip` patterns for the frame boundary.
- `feature/wip` is inspiration only, not a base: its shader probe and slot table are valid; its per-tile-uniform feeding design is the documented failure (uniforms were never wired at draw time).
- Any new hook target RVA/pattern goes through `build_manifest`, never ad-hoc constants. Known existing violation to fix: the `CInfGame` area offsets hardcoded in `hooks.cpp`.

## Repo Layout

- `src/iee/game/build_manifest.*` holds build-specific offsets, patterns, and callsites.
- `src/iee/game/tis_runtime.*` holds explicit runtime views.
- `src/iee/game/tile_upscale.*` holds scale selection logic.
- `docs/` contains the architecture and reverse-engineering notes future agents should read first.
- `docs/superpowers/specs/2026-06-10-graphics-enhancement-roadmap-design.md` is the graphics roadmap: four feature pillars, validation gates (V1-V6), and the full evaluated/dropped/rejected idea ledger. Read it before proposing any rendering feature — most ideas have already been evaluated there.
- `shaders/InfinityEngine-Enhancer.cpp` is archival research code only.

## Done Means

- Critical paths compile.
- Host-side tests pass.
- Windows-only build changes are guarded so WSL does not wander into MinHook or `windows.h` failures.
- Trade-offs and remaining Windows-only validation gaps are called out explicitly.
- Do not claim generalized build detection yet. The manifest layer is data-driven, but manifest selection is still effectively pinned to the currently validated BGEE build until explicit version/build probing is added.
