# Tile Upscale Flow

## Goal

Render explicitly supported higher-resolution tile assets while keeping the
engine's 64x64 screen-space quad and leaving ARE/WED coordinates untouched.

## Detection Order

1. Resolve the current `CVidTile` resource and demanded TIS object.
2. Read the authored TIS header tile dimension from `header + 0x14`.
3. Accept only the explicit power-of-two dimensions and map:
   - `64 -> scale 1`
   - `128 -> scale 2`
   - `256 -> scale 4`
   - `512 -> scale 8`
4. If the header is missing, inspect up to 32 entries from the 12-byte PVR table.
   Validate page/coordinate bounds, then take the GCD of non-zero `u`/`v`
   deltas between entries on the same atlas page.
5. Only if both deterministic paths fail, fall back to the legacy UV / texture-id heuristic path and log that fallback.

The header is authoritative when present. Standard tilesets can legitimately have `header == null`,
so table-based detection is also an expected deterministic path. Raw `u`/`v` origins are never tile
dimensions; only their translation-invariant grid deltas are considered. Heuristics are a final
safety net.

The value at `+0x14` is TIS metadata. The PVRZ header describes the atlas page
and does not replace this logical tile-size field. Header and table inference
use the same explicit set; arbitrary multiples of 64 are rejected.

## Generic Authored Tile Sizes

The classifier accepts `{64, 128, 256, 512}` and computes
`scale = tileDimension / 64`. Its acceptance checks are:

- Header metadata wins when present.
- Table inference must be unambiguous across sampled entries.
- Sampled coordinates must stay within the runtime's conservative atlas
  coordinate ceiling; this path does not inspect PVRZ image headers.
- The screen quad and all WED/ARE coordinates remain 64x64.
- Tests cover every supported size, malformed values, and header precedence.
- Runtime scale and linear-mode decisions are per observed tileset, held in a
  fixed-capacity cache that is cleared on every area load. Standard tilesets in
  an already-confirmed upscaled area delegate to the engine renderer rather
  than inheriting the base tileset's scale.

The cache accepts 16 tilesets per area (the WED format exposes at most eight
layers). Additional resources fail closed to the engine renderer instead of
growing memory without a bound.

The fast-disable optimization still assumes the engine's observed ordering:
the base tileset is seen before overlays. A confirmed standard base disables
the render hook for that area. Supporting arbitrary mod content that presents a
standard tileset first and a 4x tileset later would require keeping the
dispatcher installed; that broader case is not implemented because it is not a
known engine path.

## Area Scope

Cache lifetime is area-scoped; scale decisions are tileset-scoped.

- `LoadArea` resets the cached decision.
- The first `RenderTexture` calls for each observed tileset classify that tileset.
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
