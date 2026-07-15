# BGEE 2.7.3 Manifest Evidence

Status: offline validation complete (2026-07-16); in-game gates pending.
Runbook: [new-build-validation.md](../new-build-validation.md)

## Executable Identity

- Game: Baldur's Gate Enhanced Edition (Steam/Beamdog patch 2.7)
- Fixed file version: `2.7.3.0`
- ProductName / FileDescription: `Baldur's Gate Enhanced Edition`
- SHA-256: `187f89e1033999b2c8eae6591641325ba012071030364767f42221ecae92372a`
- Image base `0x140000000`, `.text` VA `0x1000`, size `0x588510`, 7 sections

## Signature Scan (executable sections only)

| Target | Pattern | Matches | 2.7.3 RVA | 2.6.6 RVA | Shift |
|---|---|---|---|---|---|
| `CInfGame::LoadArea` | unchanged 2.6.6 pattern | exactly 1 | `0x27EBD0` | `0x27E710` | `+0x4C0` |
| `CVidTile::RenderTexture` | unchanged 2.6.6 pattern | exactly 1 | `0x4257C0` | `0x4247E0` | `+0xFE0` |

Both prologues are byte-identical to 2.6.6 (24-byte dumps recorded in the
validation session log).

## Render Callsite Decode (all 11 descriptors)

Every descriptor decoded at the **same intra-function offset** as 2.6.6 with
the expected opcode, and every rel32 target resolves inside `.text`:

| Callsite | Offset | Opcode | 2.7.3 target RVA |
|---|---|---|---|
| CRes_Demand | +0x36 | E8 | 0x3F6D50 |
| DrawBindTexture | +0x6E | E8 | 0x413140 |
| DrawDisable | +0x7F | E8 | 0x413290 |
| DrawColor | +0x89 | E8 | 0x413200 |
| DrawPushState | +0x91 | E8 | 0x413450 |
| DrawColorTone | +0xB6 | E8 | 0x413220 |
| DrawBegin | +0xC0 | E8 | 0x4130B0 |
| DrawTexCoord | +0xCD | E8 | 0x413580 |
| DrawVertex | +0xDB | E8 | 0x4135A0 |
| DrawEnd | +0x17A | E8 | 0x4132D0 |
| DrawPopState (tail) | +0x1AD | E9 | 0x413430 |

## Runtime Offset Evidence

- `CVidTile::pRes = 0x100`: `mov rdi, [rcx+0x100]` present at
  `RenderTexture+0x1D`, identical to 2.6.6.
- Because all callsites sit at unchanged intra-function offsets, the
  `RenderTexture` body is byte-compatible through `+0x1B2` — strong evidence
  the `CVidTile`/tile-path layouts did not change.
- disp32 constant census over `.text` (healthy non-zero counts consistent with
  unchanged `CInfGame`/TIS layouts): `0x6590` ×434, `0x6598` ×464,
  `0x65F8` ×56, `0x1DC` ×77.
- Final layout proof is the in-game gate session below: the runtime probes
  every offset through fail-closed `safe_read` paths.

## First In-Game Run: EEex Prologue Detour (2026-07-16)

The first 2.7.3 launch failed address resolution: the manifest was selected,
but `RenderTexture` matched **zero** times in the live process while matching
exactly once on disk. A byte dump at the reference RVA explained it:

```
RenderTexture bytes at 0x4257C0: FF 25 02 00 00 00 00 00 70 E7 92 42 F9 7F 00 00 DA 48 89 68 10 ...
```

`FF 25 02 00 00 00` is `jmp qword ptr [rip+2]`, whose pointer (`0x00007FF94292E770`,
a separately-loaded DLL) is EEex's detour target. EEex — which loads this DLL —
hooks `RenderTexture` before our scan, overwriting its 14-byte prologue.
`LoadArea` is left untouched. The pattern tail from byte 16 (`DA 48 89 68 10 ...`)
is intact, and every render callsite lives at offset >= `0x36`, past the
clobbered region.

**Resolution**: `resolve_addresses` now recovers a detoured target at its
identity-gated reference RVA via `confirm_pattern_with_patched_prologue` — it
re-verifies the pattern tail past a 16-byte prologue. This never fabricates an
address on an unknown build (identity must match and the tail must still match
the original). MinHook chains cleanly onto the pre-existing EEex hook; the
in-game gates below prove the chained hook at runtime.

## In-Game Gates

Per the runbook: clean install, verbose + performance logs, standard tileset,
authored 4x area, water area, area transition, resize/fullscreen, save/load,
clean shutdown via `ShutdownBindings`.

**PASS — 2026-07-16 (maintainer confirmation).** With the detour-tolerant
build, the 2.7.3 install selects the `BGEE 2.7.3.x` manifest, recovers the
EEex-detoured `RenderTexture` at its reference RVA, and runs the enhanced
renderer: tile upscaling and WED-masked water both work. MinHook chains onto
EEex's existing hook without a mis-relocation, confirming the runtime path the
offline evidence could not. (Verdict recorded from the maintainer's in-game
session; capture a verbose+perf log here on the next run for the archive.)
