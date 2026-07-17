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
- Applies GL texture parameter upgrades and keeps a bounded 512-entry,
  deletion-aware configuration cache. Successful hits avoid driver queries;
  periodic sentinel validation covers deletion paths that bypass the observed
  GL entry point. Failed configurations are latched only until an area,
  deletion, or context reset.

`src/iee/game/tis_runtime.*`

- Provides explicit runtime views for `CRes`, `CResTile`, `CResTileSet`, `CResPVR`, and the authored TIS header.
- Reads build-specific offsets from the manifest instead of hardcoding them throughout hooks.

`src/iee/game/tis_palette.*`

- Decodes classic palette-tile transparency and derives authored liquid tint.
- Converts each palette entry to linear light once, weights area tint by opaque
  pixel count, and avoids treating a sparsely painted tile like a full tile;
  the water shader grades in the same space and re-encodes the affected result.

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
- No heuristic tier: unresolved metadata fails closed to standard 1x
  delegation (the former UV / texture-id fallback produced false 4x
  detections on vanilla areas and was removed).

`src/iee/game/are_animations.*`

- Host-safe ARE V1.0 animation-section parsing and conservative
  fire/smoke/fountain/light classification of authored ambient animations.
- The classification table grows from runtime logs of unclassified resrefs,
  not speculation. See [are-animation-detection.md](are-animation-detection.md).

`src/iee/game/object_statics.*`

- Host-safe decode of the `CGameObjectArray` globals out of the engine's
  `GetShare` body (manifest pattern + RIP-operand decode) and the read-only
  walk that collects the live `CGameStatic` records for an area.
- Sole source for the ARE-animation snapshot; any resolution ambiguity fails
  closed and disables the scan for the session.

`src/iee/hooks.*`

- Owns the runtime hook lifecycle as a thin detour -> dispatch layer.
- `LoadArea` resets area-scoped feature state; `RenderTexture` dispatches into
  the tile render feature and delegates unsupported resources to the engine.
- A frame-boundary retry makes shader-probe recovery independent of tile
  dispatch and transient RenderTexture decode failures.

`src/iee/area_state.*`

- Active-area resolution (manifest-driven CInfGame offsets), view-transform reads, and
  the post-LoadArea WED cache refresh plus render-thread area-texture queue.
- Also refreshes the ARE ambient-animation snapshot (`AppContext::areaAnimations`)
  from the live object array at the same boundary, generation-checked.
- Refresh publication is generation-checked; an older load/render callback
  cannot overwrite the newest immutable WED snapshot.
- GL objects are recreated when the current WGL context changes.

`src/iee/features/tile_render.*`

- The tile upscale render path and its per-area, fixed-capacity tileset cache,
  moved out of hooks/AppContext. Scale and linear-mode decisions are kept per
  observed tileset; the whole cache is discarded on area change.

`src/iee/diagnostics.*`

- TIS header / pointer dump logging used by detection fallbacks.

`src/iee/game/shader_override.*`

- Host-safe shader name extraction and interface-contract checks for the
  delivered `fpSEAM.glsl` game override. Unit-tested in WSL/macOS.
- Shader source delivery is performed by the game's `override` directory; this
  module does not maintain a runtime replacement registry.

`src/iee/shader_probe.*`

- Thin Windows GL detours (source/compile/link/use/delete + ARB), diagnostics,
  program classification, uniform-feed dispatch, and hook lifecycle.
- Compile/link failures are logged, but the production path does not resubmit a
  fallback shader source. The engine-owned override loading path remains the
  source of the compiled shader.
- Probe entry points and program classifications are discarded and reinstalled
  when the current WGL context changes.
- Water assets are decoded during DLL initialization, while render-thread GL
  upload stays lazy; probe hooks are queued and enabled in one MinHook batch.

`src/iee/shader_uniform_bridge.*`

- Owns atomic uniform inputs, location resolution, and render-thread
  uniform/texture binding. A state revision avoids repeating unchanged GL
  queries, sampler writes, and enhancer texture binds.
- Also carries the area's classified ambient-animation point set
  (`uIeePointCount`/`uIeePoints[32]`) with its own revision, feeding the
  fpSEAM fire/smoke/light point effects.

`src/iee/shader_diagnostics.*`

- Owns optional caller-symbol resolution and engine-shader dump file I/O.

`src/iee/frame_hook.*`

- Frame boundary via `SDL_GL_SwapWindow`, with `gdi32!SwapBuffers` as the
  validated statically-linked-SDL fallback; drives the probe's per-frame tick.

`src/iee/game/area_texture.*`

- Host-safe packing of WED per-cell liquid modes into the R8 unit-2 mask texture.
- The compact cell mask is an outer bound; base-tile alpha supplies the authored contour.

`src/iee/game/dds_texture.*`

- Host-safe, bounded parsing for single 2D BC1/BC3/BC5/BC7 DDS assets.
- Separates format validation and mip layout from the Windows/OpenGL upload path,
  so malformed-input behavior is testable without a game or GL context.

`src/iee/water_textures.*`

- Selects optional DDS assets ahead of the bundled IRGB fallback and owns their
  current-context GL objects.
- Uploads authored compressed mip chains directly and reports driver/format
  failures once per context instead of retrying on every draw.
- Discards stale GL errors immediately before its own upload sequence so an
  unrelated engine error cannot permanently block water textures for a context.

## Build Layout

The CMake graph is split on purpose:

- `iee_common` is host-safe and can be compiled in WSL for tests.
- `InfinityEngine-Enhancer` is Windows-only and links MinHook, `psapi`, and `opengl32`.
- `release_bundle` packages the DLL, EEex loader script, sample INI, shader
  assets, game override, and water textures into one directory.
- `cmake --install` mirrors the same game-root directory layout.
- `renovate.json` teaches Renovate's regex manager to update the human-readable
  spdlog/MinHook tags and their immutable `FetchContent` commit pins together.

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
