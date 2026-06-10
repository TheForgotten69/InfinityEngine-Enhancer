# Build Manifests

## Purpose

The manifest layer isolates build-specific facts from feature logic. Adding a build should mean adding data, not changing the tile-upscale algorithm or the hook flow.

## Schema

Each manifest carries:

- Build identifier
- Supported game labels
- Pattern strings for `LoadArea` and `RenderTexture`
- Fallback RVAs
- Runtime offsets
- Render callsite descriptors

## Callsite Descriptor

Each render callsite records:

- Symbol name
- Offset from `CVidTile::RenderTexture`
- Instruction kind
- Expected opcode
- Displacement offset
- Instruction size
- Whether the target is required

That format is what allows `renderer.cpp` to resolve both `CALL rel32` and `JMP rel32`.

## Current Manifest

Build id: `BGEE 2.6.6.x`

Supported games:

- `BGEE`

Fallback RVAs:

- `LoadArea = 0x27E710`
- `RenderTexture = 0x4247E0`

Runtime offsets:

- `CVidTile::pRes = 0x100`
- `TIS linear-tiles flag = 0x1DC`
- `TIS header tileDimension = 0x14`

## Adding Another Build

1. Confirm the hook target RVAs or patterns.
2. Revalidate the render callsites and instruction kinds.
3. Revalidate `CVidTile::pRes`, TIS header field offset, and the linear-tiles flag.
4. Add the new manifest entry.
5. Extend tests if the new build needs new instruction or structure cases.

If a new build cannot satisfy those checks, do not guess. Leave it unsupported and fail early.
