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

## Session 3 procedure — uniform-bridge proof (retry)

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
