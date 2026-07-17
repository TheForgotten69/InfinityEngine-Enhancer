# ARE Ambient-Animation Detection

Status: data path implemented and host-validated (in-memory CGameStatic
walk); in-game (Windows) validation pending.

## Purpose

The graphics roadmap (§10.3) lists emissives as blocked on classification:
light sources are BAM overlays mixed with map pixels, with no clean mask.
The ARE file's animation section **is** that classification channel — every
authored ambient animation (fireplaces, torches, chimney smoke, fountain
sprays, glows) is listed with exact world coordinates, a resref, a schedule,
and appearance flags.

At the `LoadArea` boundary this feature collects the active area's ambient
animations by walking the engine's live `CGameStatic` objects, classifies
them, and publishes an immutable snapshot (`AppContext::areaAnimations`).

The first consumer is the fpSEAM point-effects pass: the snapshot's shown
fire/smoke/light entries are packed into a 32-slot `uIeePoints[]` uniform
array (`build_area_effect_points`, fire prioritized under capacity pressure)
and fed through the existing uniform bridge. The shader renders warm
flickering fire glow with heat shimmer above flames, drifting chimney-smoke
haze columns, and steady candle/glow halos — background layer only, gated by
`[Shaders] EnablePointEffects` and the F10 master toggle. Effects that must
composite over sprites (true bloom) remain future P4 work consuming the same
points.

Scope boundary: **water bodies are not ARE animations.** Rivers/lakes/sea are
WED overlays and stay on the existing `wed_runtime`/`tile_liquid` path. ARE
animations cover point phenomena only (the fountain *spray*, not the pool).

## Format facts (verified)

Layout mirrored from NearInfinity (`org.infinity.resource.are.{AreResource,Animation}`)
and validated against real BG2EE data (AR0406, AR0700; see below).

- ARE V1.0 header: animation **count at +0xAC**, section **offset at +0xB0**.
  Only "AREA"/"V1.0" is accepted; IWD2 "V9.1" shifts offsets and fails closed.
- Animation record: fixed **76 bytes** —
  name[32], X u16 @+32, Y u16 @+34 (world pixels; tile = coord/64),
  schedule u32 @+36 (hour bitmask), resref[8] @+40 (BAM; EE also WBM/PVRZ),
  flags u32 @+52, height i16 @+56, translucency u16 @+58.
- Flag bits (NearInfinity EE `FLAGS_ARRAY`): bit0 `Is shown`, bit1 `No shadow`,
  bit2 `Not light source`, bit8 `Draw as background`, bit13 `EE: Use WBM`,
  bit15 `EE: Use PVRZ`. **Animations are light sources unless bit2 is set** —
  confirmed on real data: `FIRE_4` entries have bit2 clear, fly-swarm
  `FLIESS` entries have bit2 set.

## Classification

`classify_area_animation(resref, name)` → `Fire | Smoke | Fountain | Light | None`.
Conservative prefix/substring tables; unknown entries stay `None` and are
logged per area ("ARE unclassified animation resrefs for …"). **That log line
is the mechanism that grows the table from real game data** — do not pad the
table speculatively.

Evidence so far (full NearInfinity JSON exports, classified by the real
implementation):

- BG1EE (596 areas, 8,517 animations): 97.7% classified — fire 7,190,
  wildlife 633, smoke/fog/steam 199, light 174 (candle-named flames, glows),
  water 104, fountain 16, lava 5. The remaining 196 are setpiece VFX and
  static scenery (portals, dust devils, explosions, trees, drawbridges) and
  intentionally stay `None`.
- BG2EE (423 areas, 3,443 animations): 84.2% classified. The remainder is
  the long tail of the per-area `AM<area><letter>` overlay family — the
  high-count members were identified by visual frame inspection of exported
  BAMs and live in the built-in exact-resref table
  (`kExactResrefKinds`); a curated per-resref override file for the rest is
  the open proposal.
- Frame inspection also disproved a rule: `AR<area>W[DN]*` overlays are
  day/night SHADOW scenery, not lit windows. Do not classify them as light.
- Open classification questions live in the owner's
  `Documents/ARE_DATA_QUESTIONS.md` triage file.

## Primary path: in-memory CGameStatic walk

Reverse-engineered from the PDB-named Ghidra decompilation of BGEE 2.6.6.0
and cross-checked against the binary's disassembly:

- The ARE animation record is `CAreaFileStaticObject` in the engine (our
  `ARE_Animation_st`, byte-identical). During area load the engine reads
  count/+0xAC and offset/+0xB0, strides 0x4C, and news a **`CGameStatic`**
  (0x368 bytes) per record — confirming the disk parser's layout from the
  engine's own loader.
- `CGameStatic` derives from the 0x60-byte `CGameObject` base
  (`m_objectType` +0x8 — statics are type `0x30`/`'0'`; `m_pos` +0xC;
  `m_pArea` +0x18) and **retains the raw authored record at +0x60**
  (`m_header`). Script actions (`StaticStart` show/hide, palette swaps)
  mutate it in place, so the walk sees live state.
- Objects resolve through `CGameObjectArray`'s **static** globals:
  `CGameObjectArray::GetShare` (RVA `0x276490` on 2.6.6.0) bounds-checks
  `m_maxArrayIndex` (`0x68D434`) / `m_nextObjectId` (`0x68D438`) and indexes
  a fixed entry table at `0x68D450` — `{int16 objectId; CGameObject* ptr}`,
  stride 16, 15-bit index space.
- Resolution at runtime: the manifest carries EEex's version-independent
  GetShare pattern (`48 C7 02 00 00 00 00 83 F9 FF`, from
  `InfinityLoader.db`'s generic section); a unique match plus RIP-operand
  decode of `cmp WORD PTR [rip+d], ax` and `lea r8, [rip+d]`
  (`object_statics.cpp`) yields the two globals. Offline checks against the
  real binaries:
  - 2.6.6.0: one match at `0x276490`; operands decode to
    `0x68D434`/`0x68D450`.
  - 2.7.3.0 (Steam install): one match at `0x276700`, byte-identical body
    shape (same operand offsets); operands decode to `0x68F8F4`/`0x68F910`.
  - Both builds contain exactly one `operator new(0x368)` +
    `imul ..., 0x4C` site (the CGameStatic allocation in the area loader),
    confirming the object size and record stride are unchanged on 2.7.3.
- Any ambiguity (pattern count ≠ 1, duplicate/missing operands) fails closed
  and disables the scan for the session.

The walk runs fresh on every area refresh (no cache): ~`m_maxArrayIndex`
bounded reads through `safe_read`, no hooks, no writes.

A disk reader (override → `chitin.key`/BIFF) existed as a fallback during
development and was removed by owner decision once the memory path was
offline-verified on both builds — the ARE V1.0 parser (`parse_are_animations`)
remains as the host-testable format contract for the shared record layout.
Recover the locator from branch history (`key_bif.*`) if a future build
breaks the pattern.

## Threading and lifecycle

- `refresh_area_animations` runs inside `refresh_wed_cache` (LoadArea thread,
  or the render thread on post-transition re-resolution), after
  `resolve_active_area` and independent of WED parse success.
- The walk happens outside all locks; publication takes the shared commit
  mutex and re-checks the WED refresh generation, so a stale result can
  never overwrite a newer area's snapshot.
- `[Detection] AreaAnimationScan` (default `true`) gates the whole path;
  every failure is log-and-disable, never a gameplay block.

## Remaining validation (Windows/in-game)

- Confirm the memory walk in-game on 2.6.6.x: `LoadArea`-time collection
  matches the authored ARE (NearInfinity or the offline probe is the
  cross-check), and script show/hide toggles appear in refreshed snapshots.
- 2.7.3.x: GetShare pattern, globals, and the CGameStatic allocation site are
  offline-verified against the Steam 2.7.3.0 binary (see above); the
  remaining gap is the same in-game confirmation as 2.6.6.x.
- Grow the classification table from the unclassified-resref logs across a
  playthrough before any consumer ships.
