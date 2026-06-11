# Phase 0 Gate Evidence

Build: `<git sha>`   Date: `<date>`   Game: BGEE 2.6.6.x

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
