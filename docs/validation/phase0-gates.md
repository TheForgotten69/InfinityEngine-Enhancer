# Phase 0 Gate Evidence

Build: `2eae2e4`   Date: 2026-06-11   Game: BGEE 2.6.6.x (Steam)

## Session 1 verdicts (recorded from live logs)

- **V2: no compile interception at boot** — zero `glShaderSource` captures; the
  engine compiled everything before probe install. BUT the dumped `fpSEAM`
  contains the WIP-era `IEE_V1_FPSEAM_LIQUID` patch (`uIeeTileLiquidMode`,
  `uIeeLiquidTime`) — meaning **the engine reads GLSL source from game-data
  files** where that patch still lives. Shader replacement is therefore a
  game-data/override file mechanism, not (only) a compile-interception one.
  TODO: locate the shader files in the install (`dir /s *.glsl` in the game
  root, then check inside data BIFs) to confirm the load path.
- **V5: PASS** — no engine FBO binds over a full session. Our post stack can
  own FBO state.
- **Programs used in normal play: exactly three** — `fpDraw` (3, slot 0),
  `fpSEAM` (24, slot 8), `fpFONT` (18, slot 6). `fpSELECT`, `fpSprite`,
  `fpTone`, `fpCatRom` never bound. The fpSELECT proof target was therefore
  wrong; the proof retargets the fpSEAM liquid patch already in the game data.
- **V1 (partial): `uColorTone` is a vec4 uniform on `fpSEAM`** — tones look
  like uniform values, not program switches. Hypothesis "DrawColorTone selects
  programs" is likely REJECTED; confirm with bind-frequency logging.
- **Dump coverage**: retroactive introspection only reaches programs the engine
  binds. Fixed: the probe now sweeps all existing program objects at install,
  so the next session dumps everything (fpCatRom, fpSprite, fpSELECT, ...).
- Caller attribution in bind logs resolves to our own DLL (`IsActive+0xE5F0`)
  — the known `_ReturnAddress` quirk; cosmetic, fix deferred.

## Session 2 findings (2026-06-11, builds 75318e8/c18ddf5)

- **Crash on save load (75318e8)**: not reproduced on c18ddf5 after the effect
  gate defaulted OFF, the sweep moved to the frame boundary, and the legacy
  override shader was removed. Attributed to feeding liquid mode=1 on load
  into the legacy override variant; not investigated further.
- **BGEE statically links SDL** — `SDL2.dll not loaded`, so the frame hook
  never installed: no frame tick, no sweep, no F10 polling, frozen uniform
  time. Fixed: fallback hook on `gdi32!SwapBuffers` (universal WGL boundary).
- **uIee detection read the 240-byte sanitized log preview** — the engine
  prepends its header block, pushing uniform declarations past the cap, so
  registration never fired. Fixed: detection reads a 4KB un-truncated source
  prefix.
- Vanilla dumps confirmed: fpSEAM is 2.1K vanilla vs 8.3K patched. Full
  vanilla set still pending (sweep needs the working frame hook).

## Session 3 verdicts (2026-06-11, build 6cec961) — UNIFORM BRIDGE PROVEN

- **Frame hook: PASS** — `Frame hook installed on gdi32!SwapBuffers`.
- **Program sweep: PASS** — 8 pre-existing programs introspected; slot
  inference confirmed the spec table for slots 0,1,3,4,5,6,7,8 (programs
  3,6,9,12,15,18,21,24). Slot 2 (vpBlit/fpCatRom) does not exist at sweep
  time — the engine creates it on demand; post-install compile interception
  will catch it live when it appears.
- **Registration: PASS** — `Program 24 registered for uniform feed (declares
  uIee uniforms)` with the controlled game-override fpSEAM in place.
- **F10 bridge proof: PASS** — toggle ON/OFF logged and visually confirmed
  (screenshot: entire ground renders the animated liquid style; OFF restores
  baseline). The uniform feed pipeline — identification, registration,
  per-frame time, live toggle — works end to end.
- Dump coverage bug found and fixed (d96635e): swept programs dumped nothing
  because the name fallback ran after the dump call. Next session should
  dump all 8 shader pairs.
- Confirmed delivery mechanism: engine loads GLSL from the game `override/`
  folder (user-verified). Production shader delivery = override files +
  DLL uniform feeding.

## Session 4 verdicts (2026-06-11, build d96635e) — census complete

- **Dump harvest: PASS** — all 10 runtime shaders dumped by the sweep
  (fpDraw, fpFONT, fpSEAM, fpSELECT, fpSprite, fpTone, fpYUV, fpYUVGRY,
  vpDraw, vpYUV).
- **V3 verdict: fpCatRom is DEAD in 2.6.6** — `fpCatRom.GLSL` and
  `vpBlit.GLSL` exist in the game data (manually extracted) but the engine
  never compiles them: no program creation and zero compile events across a
  session with graphics options toggling, zooming, and a movie. The available
  scaling options are nearest/linear only. The planned fpCatRom replacement is
  superseded: the final-blit upgrade path is our own program + post stack (P4).
- **Bind census: all 8 runtime programs bind in normal use** — fpTone on
  pause (grey), fpYUV/fpYUVGRY in movies, fpSprite and fpSELECT during play.
  Session 1's three-program list was just a short session, not engine policy.
- **Discovery: `FPCRSPRT.GLSL` (8.6K) in game data** — a sprite zoom filter
  (gaussian via uSpriteBlurAmount/uZoomStrength) containing a **commented-out
  xBR upscaler attempt** by the engine's own developers. Never compiled at
  runtime in this session. Prime reference for Pillar 2 sprite work: the
  interface contract and the abandoned approach are both documented in it.
- Engine shader sources are Beamdog content: dumps and extractions stay in
  local reference dirs, not in this repository.

## Superseded: original Session 3 procedure

The game-data `fpSEAM` already contains the liquid patch gated on
`uIeeTileLiquidMode`. The probe now registers any program declaring `uIee*`
uniforms and feeds: `uIeeLiquidTime` = seconds, `uIeeTileLiquidMode` = F10
state (1 = on).

1. Install the new build; `EnableDebugHotkeys = true` (overrides may stay off —
   no compile interception is involved).
2. Load an area. Expect log: `Program 24 registered for uniform feed`.
3. Press F10: **the entire ground should switch to animated stylized water**;
   F10 again returns to normal. That is the end-to-end uniform bridge proof
   (slot identification + registration + per-frame time + toggle).
4. Record: log lines + short clip/screenshots ON/OFF.

Setup for all gates: install the release bundle, set in `InfinityEngine-Enhancer.ini`:

```ini
[Shaders]
EnableOverrides = false
DumpEngineShaders = true
EnableDebugHotkeys = false
```

and `EnableVerboseLogging = true`. Probes install lazily on the first area
render — load a save before checking logs.

## V2 — do our hooks see shader compilation?

Procedure: clean boot to main menu, load a save, check the log for
`GL shader source` capture lines from `detour_glShaderSource`.

- Captures on boot: YES/NO (paste first capture line or note absence)
- If NO: toggle fullscreen / change resolution in options; recompile captured? YES/NO
- Retroactive dumps present in `iee-shader-dumps/` (works regardless): list files

Verdict: PASS (compile interception works [at boot | after device reset]) / FAIL

Consequence if reset-only: the fpSELECT proof (Task 11) must use the device-reset
trigger after installing override files. Consequence if hard FAIL: STOP — Phase 1
reshapes; discuss before proceeding.

## V1 — DrawColorTone <-> glUseProgram correlation

Procedure: with verbose logging, load an area, scroll around. Paste ~20
consecutive log lines showing `DrawColorTone` activity against
`GL program bind: program=P` lines.

Note: `Detour_DrawColorTone` is currently a passthrough without logging. If no
correlation is visible from existing logs, add a temporary
`LOG_DEBUG_FAST("DrawColorTone({})", mode);` to it for this session.

- Mapping observed: tone 8 -> program `<P8>`? tone 1 -> `<P1>`?
- Does glUseProgram fire per DrawColorTone call, or batched?

Verdict: program-selector hypothesis CONFIRMED / REJECTED (+ implications)

## V5 — engine FBO usage

Procedure: full session (menu, load, area play, a movie if possible, save/load,
fullscreen toggle).

- `V5: engine bound FBO` warnings: NONE / list them

Verdict: PASS (no engine FBO use; our post stack can own FBO state) / document usage

## Slot table re-verification

From the `GL program slot inference` log lines, paste the inferred table
(program=X slot=Y vertex=vpZ fragment=fpW) and diff against spec §1's table.

## Area data texture

- `Area cell texture uploaded: WxH (unit 2, tex N)` line present after loading an
  area with water: YES/NO
- If instead `upload reported a GL error`: note it — upload moves to the frame
  tick (known caveat in area_state.cpp).

## Phase 1 proof — fpSELECT (after V2 verdict)

Procedure:
1. Copy `iee-shaders/` next to the DLL; set `EnableOverrides = true`,
   `EnableDebugHotkeys = true`.
2. The shipped `fpSELECT.glsl` is authored against the dumped original. If the log
   shows `Override fpSELECT missing interface identifier '<name>'`, the engine's
   interface differs: open `iee-shader-dumps/fpSELECT.glsl`, add/rename the listed
   identifiers in `iee-shaders/fpSELECT.glsl`, trigger recompile, repeat.
3. Trigger a recompile per V2's verdict (boot or fullscreen/resolution toggle).
4. Select a character; press F10 to A/B the ring's edge anti-aliasing.

Evidence:
- `Shader override applied: fpSELECT (...)` log line: paste
- No fallback warning after final iteration: confirm
- Screenshots ON/OFF attached/linked

## V3 — what does fpCatRom (slot 2) cover?

Procedure: set `MagentaShaders = fpCatRom`, restart, play. Note exactly what
flashes magenta (whole scene during play? movies only? the final window blit
when scaled?).

Verdict: `fpCatRom covers <X>` — decides whether the fpCatRom replacement ships.

---

## Phase 2 session — WED-masked water

Build: `<sha>`. Install the bundle DLL + copy `game-override/fpSEAM.glsl` into
the game `override/`. `EnableDebugHotkeys = true`. Use an area with water
(coast/river/harbor — check the log: `liquidOverlayMask` must be non-zero;
AR2300N was 0x00).

1. Load the area. Expect `Area liquid texture uploaded: WxH (unit 2, tex N)`
   and `Program 24 registered for uniform feed`.
2. F10 once (ON): water cells animate; land cells must be pixel-identical.
3. F10 again (ALIGN): liquid cells tint red. Walk the shoreline — the red
   region must hug the actual water edge. **Repeat while zoomed in and out**
   (this also verifies the zoom term of the world-position equation, and
   would expose any FBO-resolution mismatch). If offset: note the direction,
   whether zoom changes it, screenshot, report.
4. F10 again (OFF): everything back to vanilla.
5. Scroll and zoom during ON: waves must stay glued to the world (no swimming
   against the scroll), continuous across tile boundaries; edges of tiles show
   damped (still) water in a ~3px band — expected, not a bug.
6. Pause during ON: grey tone must still apply over the water.
7. Evidence: log + screenshots/clip per state.

Verdicts:
- Upload/registration: PASS/FAIL
- Alignment (incl. zoomed): PASS/FAIL (+offset notes)
- Water visuals: notes (incl. whether the world-space glints read as sparkle
  or blocky pop — cosmetic judgment call)
- Regression (land tiles, fonts, sprites, pause): PASS/FAIL

### Phase 2 session v2 findings (2026-06-12, AR2600)

- Gradient debug + F10 value snapshots worked exactly as designed: the log
  showed `scroll=(3, 8)`/`(17, 1)`/`(14, 60)` for a citadel view that needs
  thousands — **`nOffsetX/Y` is the within-tile smooth-scroll remainder
  (0..63), not the world view position.** The mask was anchored to the screen.
  Fixed: `read_area_scroll` now reads `m_ptCurrentPosExact` (CInfinity+0x2F4,
  layout-asserted).
- The "styling matched particles" observation: coincidence of screen position
  — with near-zero scroll the mask sampled the map's NW cells regardless of
  where the camera was.
- Viewport confirmed sane: full-window `3840x2135 at (0, 0)`; zoom values
  plausible (1.08, 1.30). The y-flip/zoom terms remain to be confirmed by the
  next gradient screenshot.
- OPEN: `m_ptCurrentPosExact` may be the view's top-left or its center. The
  v3 gradient screenshot decides: a half-viewport shift in the gradient =
  center semantics (fix is a CPU-side subtract).
- Classification quality (fountains/yspool unlit, base-TIS sea unflagged) is
  Phase 2.5: per-tile resref + pixel masks via tile_liquid machinery.

### Phase 2 session v3 findings (2026-06-12)

- `m_ptCurrentPosExact` raw values are ~14.2e6 — **16.16 fixed point** (the
  `bSetExactScale` parameter of `SetViewPosition` names the scaling; EEex docs
  give no multiplier). Geometry bounds the scale to >>16 (view x≈217, NW
  citadel ✓) or /10000 (x≈1422); >>16 shipped as the IE-standard prior.
- The uniform red wash + global waviness in v3 = mask UV in the thousands
  clamping to the far-edge texel; expected symptom, not a new bug.
- **v4 must include the scale delta-test**: press F10 (note scroll in log),
  scroll the camera right by roughly one screen width, press F10 again. The
  scroll-x delta should be ≈ viewportWidth/zoom (≈3555 at 3840/1.08). If the
  delta is ~6.5x smaller (~542), the true scale is /10000 — one-line fix.
