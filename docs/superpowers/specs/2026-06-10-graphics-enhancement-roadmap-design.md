# Graphics Enhancement Roadmap — Design

Date: 2026-06-10
Status: Draft for review
Target: BGEE 2.6.6.x (x64, Windows, OpenGL compatibility profile), EEex-loaded DLL

## 1. Context

InfinityEngine-Enhancer currently ships one feature: 4x TIS/PVRZ tile upscaling via
`CVidTile::RenderTexture` hooking with header-first scale detection. This spec defines
the next generation of graphics features and the shared infrastructure they need.

Key facts this design is built on (validated unless marked otherwise):

- The game creates a legacy `wglCreateContext` context; the driver returns an
  **OpenGL 4.6 compatibility profile**. All modern GL (GLSL 4.60, FBOs, compute,
  multi-texture) is loadable via `wglGetProcAddress`. Confirmed by the
  `feature/wip` probe (`shader_runtime.cpp`).
- The engine compiles **nine GLSL programs**, identified by filename comments in
  their source. Confirmed slot map from the live probe:

  | Slot | Vertex | Fragment | Covers |
  |---|---|---|---|
  | 0 | vpDraw | fpDraw | Generic textured quads |
  | 1 | vpDraw | fpTone | Tone (grey etc.) |
  | 2 | vpBlit | fpCatRom | Catmull-Rom blit (coverage unconfirmed — see V3) |
  | 3 | vpYUV/vpDraw | fpYUV | Movie decode |
  | 4 | vpYUV | fpYUVGRY | Movie greyscale |
  | 5 | vpDraw | fpSprite | Sprites |
  | 6 | vpDraw | fpFONT | Font glyphs |
  | 7 | vpDraw | fpSELECT | Selection circles |
  | 8 | vpDraw | fpSEAM | Tile seam path (`DrawColorTone(8)`) |

- The engine's GLSL is ARB-era (`varying`, `gl_FragColor`) and sets at least
  `uTcScale` by name on the seam program. `DrawTexCoord(int,int)` passes
  texel-space coordinates normalized in-shader.
- `ShaderTone::Seam == 8` and slot 8 is `fpSEAM`; `ShaderTone::Grey == 1` and slot 1
  is `fpTone`. Working hypothesis: **`DrawColorTone(int)` is the program selector**
  (validation gate V1).
- Fog: `CInfinity::RenderFog` (RVA `0x2A14F0`) is hookable but is setup-only.
  `CVisibilityMap::BltFogOWar3d` (RVA `0x2572D1`) **crashes on any detour**
  (stack canary, 144-byte frame) — confirmed experimentally on `feature/wip`.
  `DrawVisible` is the per-quad fog renderer and is safe to hook.
- World/UI render boundary: `CInfinity::Render(CVidMode*, int nScrollState)` and
  `CInfinity::PostRender(CVidMode*, CSearchBitmap*)` are documented symbols
  (EEex docs) — the world pass can be bracketed without touching the UI.
- Zoom factor is a direct field read: `CInfinity + 0x484 = float m_fZoom`
  (already in `eeex_doc_layouts_x64.h`).
- Sprites receive per-draw tint via
  `CInfinity::FXLockPrepForLighting(..., rgbIntensities, rgbLocalTint, ...)`.
  There is no light simulation, but there is a per-sprite tint channel.
- Day/night maps are two authored PVRZ sets; dawn/dusk are bridged by dynamic
  global tints (`ApplyNightGlobalTint`, `SetDawn/SetDusk` family). Authored maps
  are already color-graded — no global LUT work is needed or wanted.
- The engine streams tiles through a recycled VRAM pool (`AttachVRamPool`,
  `SwapVRamTiles`) and **rain invalidates tile pixels** (`InvalidateRainTiles`).
  GL texture IDs are recycled; tile contents are not immutable.
- BAM sprites are paletted and **palettes mutate at runtime** (armor tints, color
  picks, flash affects via `CVidCell::AddRangeAffect`/`AddResPaletteAffect`).
  The same logical frame uploads different RGBA bytes per palette state.
- Ghidra + the game PDB are available. New hook targets are documented symbols and
  enter the codebase as `build_manifest` pattern + fallback-RVA entries, never
  ad-hoc constants.

### Lessons from `feature/wip` (inspiration, not a base)

The branch proved the probe/patch architecture works (shader-name identification,
source patching before compile, magenta debug mode) and produced the slot table
above. It failed to render anything because the **uniform feeding bridge was never
wired**: 14 per-tile uniforms were injected into `fpSEAM` but no `glUniform*` call
ever set them. The per-tile-uniform design itself is rejected here (see §4.1).
The engine's own shaders (slot 2 in particular) are poorly written; the strategy is
to **replace** them with our own GLSL, not patch around them.

## 2. Goals

1. A reusable shader-replacement and GL-infrastructure layer (Phase 0).
2. Four feature pillars, each independently shippable and INI-gated:
   - P1: Tile shader replacement (`fpSEAM`) — animated water first.
   - P2: Sprite quality (`fpSprite`/`fpCatRom` replacement, premultiplied alpha,
     runtime GPU upscaling).
   - P3: Soft fog-of-war (bracket architecture).
   - P4: Post-processing stack (bloom, FXAA) on the world pass.
3. Every risky unknown is resolved by a named validation gate before its
   dependent feature is built.

## 3. Non-goals

- Global LUT/sharpening/vignette/grain — ReShade territory, user preference.
- Animation frame interpolation (15→60 fps) — a content-mod problem
  (interpolated BAMs + timing), not a DLL feature.
- Bindless textures / multi-draw-indirect — no measured perf problem exists.
- Shadow blobs, weather particle polish, AOE/highlight AA, lightning glow,
  dawn/dusk grading — reviewed and dropped for this cycle.
- Per-VFX bloom isolation — the post-stack bloom covers the bulk of the value.
- Offline ESRGAN for palette-mutable sprites (see §4.6 for what survives).
- Generalized multi-build support — manifest stays pinned to BGEE 2.6.6.x
  per AGENTS.md.

## 4. Architecture decisions

### 4.1 Data channel: per-frame uniforms + area data texture (not per-tile uniforms)

Per-tile uniforms are rejected: `glUniform*` requires the right program bound at
call time, `DrawBeginSort/DrawEndSort` indicate batched submission on some paths,
and per-draw uniform traffic is fragile. Instead:

- At `LoadArea`, upload the parsed WED cell classification (liquid mask, later
  any per-cell flags) as one small `R8`/`RG8` texture — one texel per WED cell —
  bound on a reserved texture unit.
- Per frame, set only frame-level uniforms (`uIeeTime`, `uIeeScroll`, `uIeeZoom`,
  `uIeeAreaMode`) once, at a point where the program is known to be bound
  (driven by V1's answer; fallback: set on first `glUseProgram` of that program
  per frame via the existing probe hook).
- Fragment shaders derive world position from `gl_FragCoord`, scroll, and zoom,
  then sample the area texture. No per-tile state anywhere. Immune to batching
  and bind-timing.

### 4.2 Shader replacement, not patching

Hook `glShaderSource` (infrastructure exists on `feature/wip`; port and harden).
When a shader is identified by name (`// fpSEAM.glsl` comment convention):

1. Dump and archive the original source (interface inventory).
2. Substitute our own complete GLSL, declared `#version 460 compatibility`.
3. **Preserve every uniform/varying name the engine queries** (`uTcScale` at
   minimum; the inventory from step 1 is the contract).

Replacements live as files in the release bundle so iteration doesn't require
recompiling the DLL. A missing/failed replacement falls back to the original
source (compile the replacement; on GL compile error, log and resubmit the
original — never ship a magenta screen to users).

### 4.3 GL state hygiene (Phase 0 prerequisite)

A RAII state-guard utility saves/restores: active texture unit, bound textures on
units we touch, current program, FBO binding, blend func, scissor/viewport.
Every detour that touches GL state uses it. The engine assumes it owns unit 0 and
the default framebuffer; one leaked binding corrupts everything.

### 4.4 Frame boundary via SDL2

The frame boundary hook is `SDL_GL_SwapWindow` from SDL2.dll's **export table** —
stable across game patches, no pattern scan. `DrawFlip` RVA is recorded in the
manifest as fallback only.

### 4.5 World/UI boundary via `CInfinity::Render`/`PostRender`

Fog compositing and UI-sensitive post-effects run inside a bracket around the
world pass, not at flip time. Both functions are documented symbols; they enter
the manifest as pattern + RVA entries. Bracket completeness is verified with the
same call-tree logging methodology as V6.

### 4.6 Sprite upscaling strategy

Palette mutation kills content-hash offline caches for creatures. The plan:

1. **v1 — runtime GPU kernel**: at upload time (`CVidBitmap::TexImage` hook or
   the GL upload call it resolves to — V4 decides the exact point), run a
   palette-agnostic single-pass upscale kernel (xBR/ScaleFX-family or
   FSR1-EASU-style) on the GPU into a 2x/4x texture under the same texture ID.
   No cache needed beyond the texture itself; works for every sprite, tint, and
   flash state. Cost is microseconds per 50x50 frame.
2. **Stretch — async ESRGAN cache**: hash-on-upload; miss renders the v1 kernel
   now and queues offline-quality upscaling to a worker; disk cache grows during
   play. Only if v1 quality disappoints.
3. Offline ESRGAN remains valid for **palette-stable** assets only (portraits,
   UI icons) and is out of scope for this cycle.

Cache/ID correctness: hook `DrawDeleteTexture` and invalidate on VRAM pool swaps
(`SwapVRamTiles`) — texture IDs are recycled (§1).

### 4.7 Halo fix is a three-part coupling

Premultiplied alpha requires all of: premultiply pixel data at upload, swap blend
func to `(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` for those draws, and sampling-aware
shader math in `fpSprite`. Shipping any subset is worse than shipping none;
the feature flag gates all three together.

## 5. Validation gates

Each gate is a logging/Ghidra experiment with a kill criterion. No dependent
feature work starts before its gate passes. Runtime questions are answered with
DLL-side logging per AGENTS.md, not Ghidra alone.

| # | Question | Method | Blocks |
|---|---|---|---|
| V1 | Is `DrawColorTone(int)` the program selector? When does `glUseProgram` fire relative to `RenderTexture`? | timestamped logs from existing probe + `DrawColorTone` hook | P1 uniform timing |
| V2 | Do our hooks install before the engine compiles shaders? | log `glShaderSource` captures on clean boot under EEex | all shader replacement |
| V3 | What does slot 2 (`vpBlit`/`fpCatRom`) cover? Final window blit? | magenta debug on `fpCatRom` | P2 blit replacement |
| V4 | Sprite UV convention: is `uTcScale` recomputed from texture dimensions per bind, or cached? What is `CVidBitmap::TexImage`'s x64 call path to GL? | Ghidra + swap a 2x test texture, observe | P2 entirely |
| V5 | Does the engine ever bind FBOs itself? | `glBindFramebuffer` probe over a session | P4 |
| V6 | Does `CInfinity::RenderFog` bracket *all* fog draws (`DrawVisible` call tree)? | call-tree logging | P3 |

(The former V7 — world/UI boundary — is resolved by §4.5: `Render`/`PostRender`
are documented; the bracket completeness check folds into V6's methodology.)

## 6. Phases

### Phase 0 — Foundation

- Port + harden the `glShaderSource`/`glUseProgram` probe from `feature/wip`
  (replace-by-name engine per §4.2, original-source archival, fallback compile).
- GL state guard (§4.3).
- Area data texture path (`LoadArea` → R8 upload) and per-frame uniform bridge.
- `SDL_GL_SwapWindow` hook (§4.4).
- Runtime A/B hotkeys (`GetAsyncKeyState`-gated, INI-configurable) — every
  feature toggleable live for visual verification.
- Hook-target prologue sanity check (EEex interop insurance).
- Run gates V1, V2, V5.
- Host-side tests: shader source replacement/name-extraction logic, GLSL string
  handling, area-texture packing — all WSL-compilable.

### Phase 1 — Proof of pipeline

- Replace `fpSELECT` with an SDF anti-aliased ring shader. Zero new data paths;
  proves identify→replace→render end-to-end. This is the pipeline's smoke test.
- Run V3; if slot 2 is the screen blit, replace `fpCatRom` with a correct
  bicubic/Lanczos kernel — global quality win at near-zero incremental cost.

### Phase 2 — Tiles (P1)

- Replacement `fpSEAM`: animated water via world-space wave field (continuous
  across tile boundaries — phase from world position, not tile-local UV),
  masked by the WED liquid texture from Phase 0. Distortion clamped to the
  tile's atlas cell to prevent neighbor bleed.
- Night-map guard: water effect parameters per area mode; no luminance-based
  effects on night maps without per-area calibration.
- Emissives and corner AO are **out** until a WED-adjacency data path exists
  (atlas-neighbor sampling is invalid — atlas layout ≠ world layout).
- Normal-map lighting is future work gated on a companion-asset pipeline; the
  shader architecture (reserved texture units, area texture) is designed not to
  preclude it.

### Phase 3 — Sprites (P2)

- Run V4 first (make-or-break for the pillar).
- Replacement `fpSprite` + premultiply-at-upload + blend swap (§4.7) behind one
  flag.
- Runtime GPU upscale kernel at upload (§4.6 v1), `DrawDeleteTexture`/pool-swap
  invalidation included.
- Async ESRGAN cache only as stretch, only if v1 quality disappoints.

### Phase 4 — Fog (P3)

- Bracket architecture: pre-`RenderFog` bind an R8 FBO sized to the viewport;
  call the original untouched (`BltFogOWar3d`/`DrawVisible` render into our
  target); post: separable blur (radius scaled by `m_fZoom`), composite over the
  scene with blue-noise dither inside the `Render`/`PostRender` world bracket.
- **Never detour `BltFogOWar3d`** (confirmed crasher).
- V6 gates this phase.

### Phase 5 — Post stack (P4)

- World-pass bracket (§4.5): FXAA on world only (UI excluded by construction).
- Bloom: bright-pass threshold → dual-Kawase blur → additive composite.
  Conservative threshold; world bracket keeps UI text out.
- Screen-space water reflection: stretch goal, only after P1's water ships and
  only if the WED water data proves sufficient at composite time.

## 7. Error handling & safety

- Every feature: independent INI flag, default conservative; failed resolution
  disables the feature, never blocks the game (matches existing repo policy:
  fail during resolution, not gameplay).
- Shader replacement: GL compile/link failure → log + fall back to original
  engine source automatically.
- All new RVAs/patterns go through `build_manifest` with pattern-first,
  RVA-fallback resolution.
- All detours use the state guard; all engine-struct reads use `safe_read`.
- WSL/Windows split preserved: GLSL string logic, replacement-engine logic, and
  data packing are host-testable; hooks and GL are Windows-only and guarded.

## 8. Testing

- WSL host tests (`iee_tests`): shader name extraction, source replacement
  contract checks (uniform/varying preservation against archived originals),
  area-texture packing, manifest entries.
- Windows runtime validation: per-gate logging experiments (V1–V6), magenta
  debug for coverage confirmation, A/B hotkeys for visual verification.
- Each phase's "done": compiles in both environments, host tests pass, gate
  evidence logged, feature visually confirmed in-game with its flag on and off.

## 9. Trade-offs accepted

- Runtime kernel upscaling (xBR/EASU) is below ESRGAN quality; accepted for v1
  because palette mutation makes offline caches unsound for creatures.
- FXAA/bloom limited to the world pass means UI gets no AA; accepted — UI is
  crisp vector/font content that AA would degrade anyway.
- Water is a fragment-shader illusion on the static PVRZ layer; the engine's
  BAM water animation continues on top, additively. No attempt to unify them.
- Replacing engine shaders couples us to their uniform interface per build;
  the archived-original contract check turns silent drift into a loud test
  failure at the next game patch.
