# Build Manifests

## Purpose

The manifest layer isolates build-specific facts from feature logic. Adding a build should mean adding data, not changing the tile-upscale algorithm or the hook flow.

## Schema

Each manifest carries:

- Build identifier
- Executable file version
- Supported game labels
- Pattern strings for `LoadArea` and `RenderTexture`
- Reference RVAs for validation and diagnostics
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

Reference RVAs (not runtime fallbacks):

- `LoadArea = 0x27E710`
- `RenderTexture = 0x4247E0`

Runtime offsets:

- `CVidTile::pRes = 0x100`
- `TIS linear-tiles flag = 0x1DC`
- `TIS header tileDimension = 0x14`

Runtime caveats for the current manifest:

- `CResTileSet::h` is not guaranteed to be populated for every tileset instance.
- Standard tilesets may expose only the PVR entry table via `CRes::pData`.
- Because of that, deterministic runtime classification for this build is header-first, then table-derived.

## Adding Another Build

Use the evidence and validation checklist in
[new-build-validation.md](new-build-validation.md). At minimum:

1. Confirm the hook target RVAs or patterns.
2. Record the executable file version and verify each signature is unique in executable sections.
3. Revalidate the render callsites and instruction kinds.
4. Revalidate every runtime offset, including active-area fields.
5. Add the new manifest entry and host tests.
6. Complete the Windows build and in-game smoke gates before claiming support.

If a new build cannot satisfy those checks, do not guess. Leave it unsupported and fail early.

## Version Gating And Self-Scanning

The runtime identifies the executable by its fixed file version and refuses to
install hooks unless that version has a known manifest and both hook signatures
are unique within the executable's code sections. This is the intended default
for unknown builds (e.g. BGEE 2.7): detect, report as unsupported, and leave
the game untouched.

The runtime cannot safely infer structure offsets and render callsites from an
arbitrary future executable — a byte pattern can be present but semantically
wrong. Supporting a new build therefore stays a two-stage process:

- automatic probe: executable version, signature match counts, target
  prologues, and render-callsite opcode validation;
- reviewed manifest: accept the build only after those results and an in-game
  smoke test agree.

A useful future feature is a `CompatibilityReport` probe-only mode (or
companion CLI) that emits one JSON/Markdown report for an unknown build without
installing hooks. Remotely downloaded manifests are not acceptable unless
signed; otherwise a compromised manifest becomes arbitrary code execution
inside the game process.
