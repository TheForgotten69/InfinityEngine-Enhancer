# Curated X64 Runtime Types

## Purpose

This project keeps a local x64 type surface so reverse-engineering and rapid prototyping do not start from raw offsets every time.

The goal is two-part:

- curated typed runtime shells for the parts we navigate directly in hooks and parsers
- exact EEex field maps for the large runtime classes that would be painful to hand-maintain as giant nested structs

The repo also carries exact file-format definitions for:

- `TIS`
- `WED`
- `BAM`
- `PVRZ`

## Provenance

- Primary reference: `EEex-Docs` x64 structure tables
- Reference snapshot used for the current curation: commit `b1a0ff3` dated `2025-08-27`
- Local runtime facts already validated in this repo still win when they are narrower or more explicit than the docs

## Modeling Rules

- Small PODs such as `CRes`, `CResRef`, `CPoint`, `CRect`, `CVidTile`, and `CVisibilityMap` are modeled directly.
- Exact file-format structs live in `src/iee/game/file_formats.h`.
- Large engine objects such as `CInfGame`, `CInfinity`, `CGameArea`, and `CGameSprite` keep exact layout with opaque gaps between the named fields we care about.
- The full documented EEex field surface for `CInfGame`, `CInfinity`, `CGameSprite`, `CGameAnimationType`, `CGameOptions`, and `CVidMode` lives in `src/iee/game/eeex_doc_layouts_x64.h`.
- Named fields should pay for themselves immediately. If a field is not helping current decoding or hook work, leave it opaque.
- Any new curated field should land with `sizeof` / `offsetof` coverage in host tests.
- Any new EEex field-map mirror should land with row-count and spot-check coverage in host tests.

## Why Not Import The Whole Docs Repo

- It is mostly documentation, not compiler-ready headers.
- Most of the repo is irrelevant to the current DLL.
- A full mirror would add a lot of surface area without improving validated behavior.
- Curated local layouts are easier to audit when build-specific runtime facts change.

## Current Coverage

- Common runtime resources: `CRes*`, `CResRef`, `TisFileHeader`
- Video / renderer objects: `CVidPalette`, `CVidImage`, `CVidTile`, `CVidCell`, `CVidMode`, `CVidPoly`
- Tile / world glue: `CInfTileSet`, `CInfinity`, `CVisibilityMap`, `CTiledObject`
- Game-state anchors: `CInfGame`, `CGameArea`, `CGameSprite`
- Exact file/resource formats: `TIS`, `WED`, `BAM`, `PVRZ`
- Exact documented field maps: `CInfGame`, `CInfinity`, `CGameSprite`, `CGameAnimationType`, `CGameOptions`, `CVidMode`
