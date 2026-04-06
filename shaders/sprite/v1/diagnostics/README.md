# Sprite Diagnostics

These probe shaders are intentionally ugly. They are for answering runtime questions, not for quality.

Use them when a subtle filter change is too weak to tell whether the engine is actually:

- loading the replacement shader file
- binding `uTcScale`
- routing actor bodies through `fpDraw`

## Meaning

For solid sprite pixels:

- bright green means `uTcScale` is non-zero
- bright red means `uTcScale` is zero or missing

For the EASU delta diagnostic:

- black or nearly black means the one-pass EASU path is changing very little
- blue means a small change
- orange means a moderate change
- white means a large change

If the sprite appearance does not change at all after replacing the live shader with one of these probes, the engine is likely not loading the edited file you replaced.

For the `fpDraw` path probe:

- dark blue content means the generic `fpDraw` path is active there
- hot pink / cyan striped content means the generic path is active there and the content also looks "sprite-like" based on nearby alpha discontinuities
- if actor bodies do not change under the `fpDraw` path probe, they are almost certainly not going through `fpDraw`

## Files

- `fpsprite_probe.glsl`
- `fpsprite_easu_delta.glsl`
- `fpselect_probe.glsl`
- `fpselect_easu_delta.glsl`
- `fpdraw_path_probe.glsl`

Replace the live game shader temporarily, take a screenshot, then restore the baseline or v1 file.

Notes:

- Unselected actors and ground sprites are most likely to exercise `fpsprite`.
- Selected actors often go through `fpselect`, which still renders solid sprite texels before handling outline pixels.
- `fpdraw_path_probe.glsl` is intentionally broad and should only be used as a routing probe, not as a feature candidate.
