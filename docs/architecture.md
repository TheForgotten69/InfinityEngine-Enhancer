# Architecture

## Scope

The supported runtime is intentionally narrow: one EEex-loaded Windows DLL, one validated BGEE build manifest, and one feature set centered on tile upscaling. The code is structured so additional game/build support is a data expansion exercise, not another rewrite.

## Runtime Modules

`src/iee/game/build_manifest.*`

- Declares the executable version, build-specific patterns, reference RVAs, runtime offsets, and verified render callsites.
- The current manifest is BGEE `2.6.6.x`.

`src/iee/game/game_addrs.*`

- Resolves `CInfGame::LoadArea` and `CVidTile::RenderTexture`.
- Scans executable PE sections and accepts only one match for each required signature.
- Reference RVAs are diagnostic evidence only; missing or ambiguous signatures abort initialization.

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
- Headerless PVR tables are classified from same-page coordinate deltas, not raw atlas origins.
- UV / texture-id heuristics remain fallback only.

`src/iee/hooks.*`

- Owns the runtime hook lifecycle as a thin detour -> dispatch layer.
- `LoadArea` resets area-scoped feature state; `RenderTexture` dispatches into
  the tile render feature and honors feature hook-disable requests.

`src/iee/area_state.*`

- Active-area resolution (manifest-driven CInfGame offsets), view-transform reads, and
  the post-LoadArea WED cache refresh plus render-thread area-texture queue.
- GL objects are recreated when the current WGL context changes.

`src/iee/features/tile_render.*`

- The tile upscale render path and its per-area state, moved out of hooks/AppContext.

`src/iee/diagnostics.*`

- TIS header / pointer dump logging used by detection fallbacks.

`src/iee/game/shader_override.*`

- Host-safe shader replacement logic: name extraction, override registry,
  magenta debug variants, interface-contract checks. Unit-tested in WSL/macOS.

`src/iee/shader_probe.*`

- Thin Windows GL detours (source/compile/link/use/delete + ARB), program classification,
  compile-failure fallback, and hook lifecycle.

`src/iee/shader_uniform_bridge.*`

- Owns atomic uniform inputs, location resolution, and render-thread uniform/texture binding.

`src/iee/shader_diagnostics.*`

- Owns optional caller-symbol resolution and engine-shader dump file I/O.

`src/iee/frame_hook.*`

- Frame boundary via `SDL_GL_SwapWindow`, with `gdi32!SwapBuffers` as the
  validated statically-linked-SDL fallback; drives the probe's per-frame tick.

`src/iee/game/area_texture.*`

- Host-safe packing of WED per-cell liquid modes into the R8 unit-2 mask texture.
- The compact cell mask is an outer bound; base-tile alpha supplies the authored contour.

## Build Layout

The CMake graph is split on purpose:

- `iee_common` is host-safe and can be compiled in WSL for tests.
- `InfinityEngine-Enhancer` is Windows-only and links MinHook, `psapi`, and `opengl32`.
- `release_bundle` packages the DLL, EEex loader script, sample INI, shader
  assets, game override, and water textures into one directory.

This keeps reverse-engineering and parsing code testable without requiring a Windows toolchain for every edit.

## Operational Boundaries

- Use WSL for code analysis, docs, and host tests.
- Use Windows or CI for final DLL validation.
- Unknown or incompatible builds should fail during manifest/address/callsite resolution instead of reaching gameplay with bad hooks.
- A loader that calls `FreeLibrary` must invoke exported `ShutdownBindings`
  first; MinHook, logging, and GL teardown are deliberately never run from
  `DllMain` under the Windows loader lock.
- [threading-model.md](threading-model.md) defines callback ownership and exception boundaries.

## Archival Code

[docs/archive/prototypes](archive/prototypes/README.md) preserves unsupported experiments. Nothing
in that directory is part of the build or a source of truth for runtime behavior.
