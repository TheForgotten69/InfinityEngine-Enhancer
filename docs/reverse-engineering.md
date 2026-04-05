# Reverse Engineering Notes

## Workflow Rules

- Record RVAs, not absolute runtime VAs.
- Runtime VA = module base + RVA.
- Ghidra, PDB, and in-game logs can disagree on absolute addresses because of ASLR and function chunking.
- Treat EEex-documented layouts as authoritative when they match runtime behavior.
- Treat build-manifest offsets as locally validated runtime facts that may need revalidation per build.

## Hook Targets

Use `CVidTile::RenderTexture`, not either `CVidCell::RenderTexture` overload.

- `CInfGame::LoadArea`
  - RVA `0x27E710`
- `CVidTile::RenderTexture`
  - RVA `0x4247E0`

`LoadArea` is the area boundary. It resets scale detection and allows `RenderTexture` to reclassify the next area.

## Verified `CVidTile::RenderTexture` Callsites

Current build: BGEE `2.6.6.x`

| Symbol | Offset | Kind | Opcode |
| --- | ---: | --- | ---: |
| `CRes::Demand` | `0x36` | `call_rel32` | `0xE8` |
| `DrawBindTexture` | `0x6E` | `call_rel32` | `0xE8` |
| `DrawDisable` | `0x7F` | `call_rel32` | `0xE8` |
| `DrawColor` | `0x89` | `call_rel32` | `0xE8` |
| `DrawPushState` | `0x91` | `call_rel32` | `0xE8` |
| `DrawColorTone` | `0xB6` | `call_rel32` | `0xE8` |
| `DrawBegin` | `0xC0` | `call_rel32` | `0xE8` |
| `DrawTexCoord` | `0xCD` | `call_rel32` | `0xE8` |
| `DrawVertex` | `0xDB` | `call_rel32` | `0xE8` |
| `DrawEnd` | `0x17A` | `call_rel32` | `0xE8` |
| `DrawPopState` | `0x1AD` | `jmp_rel32` | `0xE9` |

The last item matters: `DrawPopState` is a tail jump, so a resolver that assumes every branch is `E8 rel32` is wrong on this build.

## Verified Runtime Offsets

Current build manifest values:

- `CVidTile::pRes = 0x100`
- `TIS linear-tiles flag = 0x1DC`
- `TIS header tile-dimension field = 0x14`

## Structure Provenance

Authoritative from EEex docs where they line up with runtime behavior:

- `CRes::pData` at `+0x40`
- `CRes::bLoaded` at `+0x51`
- `CResTile`
- `CResTileSet`
- `CResPVR`

Locally validated for this project/build:

- `CVidTile::pRes`
- TIS linear-tiles flag at `+0x1DC`
- `CVidTile::RenderTexture` callsite offsets and instruction kinds
- `CResTileSet::h` being optional at runtime

## Tile Detection Facts

- TIS header field `+0x14` is the authored tile dimension used by the asset pipeline.
- `0x40` means standard tiles.
- `0x100` means authored 4x tiles.
- `CResTileSet::h` is present for at least the authored 4x path, but can be null for standard tilesets.
- When `h` is null, the 12-byte PVR entry table in `pData` is still sufficient to classify scale deterministically by scanning the smallest positive `u`/`v` step.
- The `+0x1DC` flag is kept only for the current seam/linear tone handling path, not for deciding upscale factor.
