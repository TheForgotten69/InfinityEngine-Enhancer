# Tile Upscale Flow

## Goal

Render authored 4x tile assets while keeping the engine's 64x64 screen-space quad and leaving ARE/WED coordinates untouched.

## Detection Order

1. Resolve the current `CVidTile` resource and demanded TIS object.
2. Read the authored TIS header tile dimension from `header + 0x14`.
3. Map:
   - `0x40 -> scale 1`
   - `0x100 -> scale 4`
4. If the header is missing, inspect up to 32 entries from the 12-byte PVR table.
   Validate page/coordinate bounds, then take the GCD of non-zero `u`/`v`
   deltas between entries on the same atlas page.
5. Only if both deterministic paths fail, fall back to the legacy UV / texture-id heuristic path and log that fallback.

The header is authoritative when present. Standard tilesets can legitimately have `header == null`,
so table-based detection is also an expected deterministic path. Raw `u`/`v` origins are never tile
dimensions; only their translation-invariant grid deltas are considered. Heuristics are a final
safety net.

The value at `+0x14` is TIS metadata. The PVRZ header describes the atlas page
and does not replace this logical tile-size field. The current implementation
supports 64- and 256-pixel authored tiles.

## Future: Generic Authored Tile Sizes (64/128/256/512)

The classifier should be generalized to accept the explicit power-of-two set
`{64, 128, 256, 512}`, compute `scale = tileDimension / 64`, and use the same
accepted values for the PVR entry-table fallback. Do not accept arbitrary
dimensions merely because they are divisible by 64.

Acceptance checks for this feature:

- Header metadata wins when present.
- Table inference must be unambiguous across sampled entries.
- `u + tileDimension` and `v + tileDimension` must remain within the actual
  PVRZ atlas dimensions.
- The screen quad and all WED/ARE coordinates remain 64x64.
- Tests cover every size, atlas edges, malformed counts, and mixed metadata.
- Runtime state is per tileset, not just per area, so a mod with mixed TIS
  resources cannot inherit the first tileset's scale.

The last point is the main architectural follow-up: the existing area-wide
scale cache is sufficient for ordinary maps but too coarse for fully general
mod content.

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
