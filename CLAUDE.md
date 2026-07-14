# CLAUDE

Operational instructions for AI coding agents working in this repository.

## Audience And Constraints

- Write for a principal or senior engineer.
- Prefer KISS, YAGNI, DRY, and SOLID pragmatically.
- Do not add dependencies or architectural layers unless they pay for themselves immediately.

## Environment

- macOS (no CMake toolchain installed): analysis, docs, and reverse-engineering work only — rely on CI for build/test verification.
- WSL: analysis, docs, and host-side tests.
- Windows or CI: the final DLL build and runtime validation.
- CI (`.github/workflows/build.yml`) runs the same commands below on every push; a green run is valid build/test evidence when no local toolchain exists.
- The supported runtime target today is BGEE `2.6.6.x`.

## Build And Test Commands

- WSL host tests:
  `cmake -S . -B cmake-build-debug -DBUILD_TESTING=ON`
  `cmake --build cmake-build-debug --target iee_tests`
  `ctest --test-dir cmake-build-debug --output-on-failure`
- Windows DLL build:
  `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DIEE_BUILD_WINDOWS_DLL=ON -DBUILD_TESTING=ON`
  `cmake --build build --config Release --target release_bundle`

## Runtime Facts — Tiles / TIS

- TIS header `+0x14` is the authoritative tile-dimension field.
- Explicit `64`, `128`, `256`, and `512` dimensions map to `1x`, `2x`, `4x`,
  and `8x`. Other values fail closed to the existing fallback path.
- `CResTileSet::h` is optional. Standard tilesets can have `header == null` while still exposing a valid 12-byte PVR entry table through `pData`. A null header does not imply missing deterministic metadata.
- Deterministic detection order for this build is:
  `TIS header -> PVR entry table coordinate-grid GCD -> legacy heuristic fallback`
- `+0x1DC` is only the current linear-tiles tone flag for this build.

## Runtime Facts — Renderer / GL

- The renderer is an OpenGL 4.6 **compatibility profile** context (legacy `wglCreateContext`; modern GL loads via `wglGetProcAddress`).
- The engine compiles nine named GLSL programs (`fpSEAM`, `fpSprite`, `fpSELECT`, `fpCatRom`, ...). The verified slot table and shader-replacement strategy live in the graphics roadmap spec (see Docs).
- The engine does **zero** lighting work. Day/night is a swap of authored assets. `CGameArea` holds authored per-area bitmaps (`m_bmLum` +0x260, `m_pbmLumNight` +0x380, `m_bmHeight` +0x388) usable as static lookup data only.
- Prefer `SDL_GL_SwapWindow` (SDL2.dll export) over `DrawFlip` patterns for the frame boundary.

## Runtime Facts — Hooking Safety

- Hook `CVidTile::RenderTexture`, not `CVidCell::RenderTexture`.
- `LoadArea` is the area boundary and resets scale detection.
- **Never detour `CVisibilityMap::BltFogOWar3d`** — crashes on any modification (stack canary, confirmed experimentally). `CInfinity::RenderFog` is setup-only but is safe to hook as a bracket around the fog pass.
- Any new hook target RVA/pattern or build-specific runtime offset goes through
  `build_manifest`, never ad-hoc constants. The `CInfGame` visible-area, area
  array, and master-area offsets already follow this rule.
- `feature/wip` is inspiration only, not a base: its shader probe and slot table are valid; its per-tile-uniform feeding design is the documented failure (uniforms were never wired at draw time).
- If you need live runtime facts, add DLL logging. Ghidra is useful for offline layout review but cannot answer live process-state questions on its own.

## Repo Layout

- `src/iee/game/build_manifest.*` holds build-specific offsets, patterns, and callsites.
- `src/iee/game/tis_runtime.*` holds explicit runtime views.
- `src/iee/game/tile_upscale.*` holds scale selection logic.
- `docs/` contains the architecture and reverse-engineering notes future agents should read first.
- `docs/threading-model.md` defines callback ownership, GL-thread rules, and ABI exception boundaries.
- `docs/superpowers/specs/2026-06-10-graphics-enhancement-roadmap-design.md` is the graphics roadmap: four feature pillars, validation gates (V1-V6), and the full evaluated/dropped/rejected idea ledger. Read it before proposing any rendering feature — most ideas have already been evaluated there.
- Unsupported experiments live under `docs/archive/prototypes/` and are never production sources.

## Done Means

- Critical paths compile.
- Host-side tests pass.
- Windows-only build changes are guarded so WSL does not wander into MinHook or `windows.h` failures.
- Trade-offs and remaining Windows-only validation gaps are called out explicitly.
- Do not claim generalized build detection yet. The manifest layer is data-driven, but manifest selection is still effectively pinned to the currently validated BGEE build until explicit version/build probing is added.
