# Tile Upscale Flow

## Goal

Render authored 4x tile assets while keeping the engine's 64x64 screen-space quad and leaving ARE/WED coordinates untouched.

## Detection Order

1. Resolve the current `CVidTile` resource and demanded TIS object.
2. Read the authored TIS header tile dimension from `header + 0x14`.
3. Map:
   - `0x40 -> scale 1`
   - `0x100 -> scale 4`
4. If the header is missing, inspect the 12-byte PVR entry table and classify by the smallest positive `u`/`v` step.
5. Only if both deterministic paths fail, fall back to the legacy UV / texture-id heuristic path and log that fallback.

The header is authoritative when present. Standard tilesets can legitimately have `header == null`, so table-based detection is also an expected deterministic path. Heuristics are there only as a final safety net.

## Area Scope

Scale detection is area-scoped.

- `LoadArea` resets the cached decision.
- The first `RenderTexture` calls in the new area classify the tileset.
- Once a standard area is confirmed, the render hook disables itself for that area to avoid unnecessary overhead.

## Render Path

The hook keeps the on-screen quad at `64x64`, but expands UV span according to the detected scale factor.

For 4x tiles:

- Screen quad remains `64x64`
- UV span becomes `256x256`

That preserves lighting, clipping, and engine coordinate expectations while still sampling the authored higher-resolution tile.

## Tone Handling

The current build still uses the TIS `+0x1DC` linear-tiles switch for the seam/linear tone path. That flag is not used for scale classification.

## Failure Model

- Missing manifest or incompatible callsites abort initialization.
- Missing tile metadata falls back to heuristics.
- Unknown builds should stop during initialization rather than silently running with invalid hooks.
