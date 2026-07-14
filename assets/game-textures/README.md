# Water Texture Assets

## Runtime Formats

For each texture stem, the runtime prefers a `.dds` file when one is present
and otherwise loads the bundled `.rgba` file. A present but invalid DDS fails
closed with a specific log entry; it is not silently hidden by the raw fallback.

The DDS loader deliberately supports only one two-dimensional texture in these
block-compressed formats:

- BC1/DXT1 (legacy DDS or DX10 header; UNORM and DX10 sRGB)
- BC3/DXT5 (legacy DDS or DX10 header; UNORM and DX10 sRGB)
- BC5/ATI2/BC5U (legacy DDS or DX10 header; UNORM)
- BC7 (DX10 header; UNORM or sRGB)

Texture arrays, cubemaps, volume textures, BC2/DXT3, BC4, BC6H, signed formats,
and uncompressed DDS are rejected. A supplied mip chain is uploaded directly.
For a single-level DDS, the runtime asks OpenGL to generate the remaining mips;
if that entry point is unavailable, it safely uses base-level linear filtering.
If the current driver rejects a selected compressed format, the log identifies
the asset and format; remove that DDS override to return to the bundled raw
fallback on the next launch.

Use the same stems as the raw assets:

- `iee_water_normal.dds`
- `iee_water_dudv.dds`
- `iee_water_foam.dds`

BC5 is the recommended space/quality trade-off for normal and DuDv data. BC7
provides the best of the supported quality options for RGBA content. BC1 can be
reasonable for the foam mask if visible block artifacts are acceptable; BC3 is
the middle ground when alpha quality matters. These shader inputs are data, not
display-color images, so export them as linear/UNORM rather than sRGB unless the
shader contract is intentionally changed to expect sRGB decoding.

## Bundled Raw Fallback

The runtime blob format is `IRGB`, followed by little-endian `u32` width and
height and then `width * height * 4` RGBA8 bytes.

Current dimensions:

- `iee_water_normal.rgba`: 512x512 tiling normal map
- `iee_water_dudv.rgba`: 512x512 tiling DuDv map
- `iee_water_foam.rgba`: 1024x1024 tiling foam mask

The runtime generates mipmaps and uses trilinear minification when the current
OpenGL context exposes `glGenerateMipmap`. Replacements should therefore focus
on seamless tiling and visual fit rather than increasing resolution.

The current resolutions are sufficient for the shader's sampling scales. The
assets are serviceable, not a locked art baseline: a replacement set should
use independent normal and DuDv patterns, avoid obvious repetition,
tile cleanly on both axes, and keep the foam mask useful after minification.

## Asset Provenance

The current three raw blobs were generated specifically for this project by the
project owner; they were not imported from another game or mod.

Before importing any replacement from another game or mod, record its author,
source, license, and redistribution permission here. Do not assume that
availability in a mod grants permission to redistribute it under this
repository's license.
