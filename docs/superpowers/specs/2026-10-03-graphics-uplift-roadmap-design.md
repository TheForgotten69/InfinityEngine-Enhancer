# Graphics Uplift Roadmap — Feasibility and Order

Date: 2026-10-03
Status: Feasibility record and ordering. Each item still needs its own spec
before implementation.
Target: BGEE 2.7.3.x (x64, Proton), EEex-loaded DLL, EEex Uncap FPS enabled.

This document supersedes the *scope decisions* of
[2026-06-10-graphics-enhancement-roadmap-design.md](2026-06-10-graphics-enhancement-roadmap-design.md)
where they conflict (see [Reversed decisions](#reversed-decisions)). The
architecture rules in that document and in `docs/architecture.md` still hold:
manifest-only addresses, GL state guard, fail closed, shader delivery through
the game `override/` directory plus DLL uniform feeding.

## 1. Evidence base

All RVAs below are for the installed Steam `Baldur.exe` 2.7.3. Its CodeView
record (GUID `B671EED8-BE5E-4979-A639-3E707E611587`, age 75) matches
`IE_2.7.3.0_Win64_debug/Baldur.pdb`, so PDB symbols are authoritative for this
build. Decompiles came from a headless Ghidra project with the PDB applied
(location recorded in the workspace `AGENTS.md`). Everything here is static
analysis; items marked **gate** need DLL-side logging before they are relied on.

### 1.1 Timing

| Fact | Source |
|---|---|
| Logic ticks at 30 Hz | `CChitin::TIMER_UPDATES_PER_SECOND` (RVA `0x65CC5C`) = 30 |
| VEF/VVC frame pacing is derived from 30 | `CVisualEffect::MAX_FRAME_RATE` (RVA `0x59EC20`) = 30, used in `CVEFVidCell::FrameAdvance` `0x254440` |
| Rendering runs faster than logic | EEex Uncap FPS (`CChitin::Override_Update`), limit 240 in the local config |
| EEex smooths only the viewport | `EEex_UncapFPS_Patch.lua` hooks scroll, zoom, screen shake, tooltips; no sprite hooks. `EEex.dll` exports no tick-progress value |
| EEex also detours `CVidTile::RenderTexture` | `Hook-CVidTile::RenderTexture()-FirstInstruction` — same function this mod hooks |

### 1.2 Sprite and effect draw path

Every paletted BAM frame is rebuilt on the CPU on every rendered frame:

```
CVidCell::Render3d (0x424780 / 0x424AE0)
  GetFrame (vtable, 0x4118A0)            m_nCurrentSequence/m_nCurrentFrame -> m_pFrame
  CVidPalette::Realize -> rgbTempPal     live palette (tints, range affects) applied here
  DrawLockSurface (GL: 0x42CAF0)         slot in the streaming atlas fx[0] nearest / fx[1] linear
  CVidCell::Blt8To32 (0x426C00)          8-bit -> RGBA into the atlas texels
  CVidCell::RenderTexture (0x425530)     blend mode from flags, DrawQuad
```

Characters and effects composite several cells into one surface first:

```
CInfinity::FXPrep 0x29E1B0 -> FXLock 0x29E190
  FXRender 0x29E260 per cell             body, helmet, shield, weapon (order by facing)
  FXRenderClippingPolys 0x29E4C0         wall polygons rasterised into the surface
FXUnlock 0x29EA40 -> FXBltFromClipped 0x29DFF0 -> CVidMode::FXBltToBack
```

Consequences:

- The GPU receives one RGBA image per sprite with the live palette already
  applied. Runtime palette mutation is therefore **not** an obstacle for
  anything done at or after `Blt8To32`.
- Sprites share a streaming atlas. Any shader kernel that samples neighbours
  must clamp to the quad's own rectangle.
- BAM v2 (PVRZ-backed) frames take `CVidCell::RenderPVR` (`0x4250D0`) instead
  and skip the CPU composite.
- The sprite shadow is one palette index (`CVidPalette::SHADOW_ENTRY`),
  resolved in `Render3d` before the blit.
- Wall occlusion can use dithered spans
  (`CVidPoly::DrawHLineDithered32` `0x42DB50`, "Always Dither" option).

Per-object render entry points that route into the path above:

| Object | Function | RVA |
|---|---|---|
| Creature | `CGameSprite::Render` | `0x36BA50` |
| Character animation | `CGameAnimationTypeCharacter::Render` (+16 sibling types) | `0x32C240` |
| VEF/VVC effect cell | `CVEFVidCell::Render` | `0x254B00` |
| Effect animation | `CGameAnimationTypeEffect::Render` | `0x32D020` |
| Projectile | `CProjectileBAM::Render` | `0x233F20` |
| Fireball | `CGameFireball3d::Render` | `0x1EAF90` |
| Ambient static | `CGameStatic::Render` (already hooked) | `0x1F27D0` |
| Particles (GL points) | `CParticle::Render` | `0x425BE0` |
| Rain / snow | `CRainStorm::Render` / `CSnowStorm::Render` | `0x25CD20` / `0x25CDD0` |

### 1.3 Frame composition order

`CGameArea::Render` (`0x189360`): `CInfinity::Render` (tiles) →
`RenderEdgeFade` → object lists (virtual `Render`) → `RenderTransitions` →
`RenderAOE` → `RenderFog` → `PostRender`. UI draws afterwards, so a bracket
around this function excludes the UI by construction.

`CInfinity::RenderFog` (`0x2A1B60`) draws fog as untextured, alpha-blended
triangles per 64 px tile inside one `DrawBegin`/`DrawEnd`. It is safe to
bracket; `CVisibilityMap::BltFogOWar3d` (`0x257520`) must still never be
detoured.

### 1.4 Engine light value

`CInfinity::Render` multiplies every tile by `m_rgbGlobalLighting`
(`GetGlobalLighting` `0x29F890`: time-of-day, overcast and lightning colours
combined) and hands the same colour to sprites via `CVidMode::rgbGlobalTint`.
`CGameArea::GetTintColor` (`0x182ED0`) adds the per-position lightmap tint for
sprites. This is a flat colour, not a light simulation.

### 1.5 Engine shaders

`chitin.key` lists `FPSPRITE`, `FPCRSPRT`, `fpDraw`, `fpSeam`, `FPSELECT`,
`FPFONT`, `fpTone`, `fpYUV`, `FPYUVGRY`, `fpCatRom`, `vpDraw`, `vpBlit`,
`vpYUV` (and `FPIT1L/M/S`). Originals are extractable from the BIFs for
interface contracts. Extracted sources are Beamdog content and stay out of
this repository.

## 2. Items

Each item lists what it does, where it attaches, and the main risk.

### A. Position smoothing

Sprites move in 30 Hz steps while the camera scrolls at display rate. Lerp
each sprite's draw position between ticks inside a `CGameSprite::Render` hook
(swap position in, call original, restore).

- Implemented 2026-10-03 behind `[Rendering] SmoothSpriteMovement` (default
  off, 2.7.3 only); in-game validation pending.
- No engine "previous position" is used: `SpriteMotionTracker` observes each
  sprite's `m_pos` at render time, treats a change as a logic tick, and slides
  from the last shown position over the interval between the last two
  changes (30 Hz assumed until observed). No tick hook is needed.
- `CGameSprite::RenderMarkers` (`0x36F170`) and `RenderHealthBar`
  (`0x36E820`) are wrapped with the same swap and share one per-frame
  timestamp, so circles and bars stay glued to the sprite.
- Known limits: positions are whole world pixels; projectiles and effects
  still step at the logic rate; anything else that reads `m_pos` during
  render (floating text, action icons drawn outside the three hooks) is not
  smoothed.

### B. `fpSprite` replacement

One override shader that covers every character and effect quad.

- B1 upscaling kernel (pixel-art family; `FPCRSPRT` holds the developers' own
  abandoned xBR attempt as a reference).
- B2 premultiplied-alpha halo fix (all three parts together, per the 2026-06-10
  design §4.8).
- B3 soft shadows: tag `SHADOW_ENTRY` pixels at blit time, blur in shader.
- B4 smooth wall occlusion: replace the dithered stipple with alpha.
- Risk: atlas neighbour bleed; **gate** on per-quad UV rectangle availability.

### C. Effect replacement framework

Hook the per-object render, look the resref up in a table, then pass through
or suppress and draw the replacement. The hook seam is the shipped
`CGameStatic::Render` pattern, generalised.

Where the replacement is drawn is a separate choice. Today fire and smoke are
painted inside `fpSEAM` (the tile pass), which has structural limits: they
sit under every object, are multiplied by the global light colour, and cost
a per-pixel loop over the point set. Drawing them with our own program at
the engine's final blit (`CVidMode::FXBltToBack` `0x41D540`) would remove
those limits, but needs the engine's internal batch flush pinned first.
Owner decision 2026-10-03: stay in `fpSEAM` for now and fix placement there.

- C1 fix placement and size of the existing smoke / fire replacements
  (current branch): geometry now comes from the BAM frame table, see
  `docs/are-animation-detection.md`.
- C2 extend to VEF/VVC, projectiles, fireballs; first batch is the
  highest-frequency effects, the table grows from logs of unreplaced resrefs.
- C3 effect-cast light: publish active effect positions into the existing
  point set so effects light tiles and sprites.
- C4 particles and weather (`CParticle`, rain, snow) as shader passes.
- **Gate:** resref and world-position field offsets per class (PDB types /
  EEex docs → `build_manifest`).
- Risk: "all effects" is content work; it is never finished, only prioritised.

### D. Offscreen world pass

- D1 soft fog of war: render `RenderFog` into an offscreen target, blur
  (radius scaled by zoom), composite.
- D2 bloom on the world pass only.
- D3 water reflections of sprites and effects.
- Precondition already met: the engine binds no FBOs (gate V5, 2026-06-11).

### E. Companion textures per tile page

A sibling texture per PVRZ page, generated offline alongside the upscale.

- E1 normal maps → per-pixel relighting from the point set (C3).
- E2 wall-polygon occlusion for those lights, and light-directed sprite
  shadows. The polygons are the ones `FXRenderClippingPolys` consumes.
- E3 "living maps": foliage sway and window/banner flicker from offline masks.
- Risk: quality of colour-derived normals and segmentation masks on painted
  art. Prototype offline on a few areas before any loader work.

### F. Animation frame in-betweening

At the sprite render, decode frame N and N+1 with the same live palette and
blend in our own shader by tick progress.

- Costs one extra CPU blit per sprite and a small animation latency.
- Crossfade ghosts on fast limb motion; motion-compensated blending is
  plausible at ~100 px but unproven on this art.
- Starts as an offline spike on extracted BAM frames. No DLL work until the
  spike shows acceptable quality.
- PVRZ-backed sprites need a separate path.

### G. Small polish

Anti-aliased selection rings / AOE markers / path lines (`fpSELECT`), cutscene
scaling kernel (`fpYUV`), offline upscaling of palette-stable portraits and
item icons.

## 3. Order

| # | Item | Why here |
|---|---|---|
| 1 | C1 | Existing work on the branch; make it correct before building on it |
| 2 | A | One hook, most certain perceived gain |
| 3 | B1, B2 | Sprites are the most visible mismatch against 4x maps |
| 4 | C2, C3 | Effects plus the light they cast |
| 5 | D1, D2 | Shared offscreen plumbing |
| 6 | B3, B4 | Ride on the `fpSprite` replacement |
| 7 | F spike | Quality question answered offline first |
| 8 | E1 → E2 → E3 | Largest visual jump, largest asset cost |
| 9 | C4, D3, G | Polish |

## 4. Reversed decisions

| 2026-06-10 decision | Now | Reason |
|---|---|---|
| Frame interpolation rejected as a content-mod problem (§10.8) | Item F, spike first | Frames are composited on the CPU with the live palette; the palette-mutation objection does not apply at the blit |
| Per-VFX work dropped as disproportionate (§3, §10.7) | Item C | Per-object `Render` hooks give resref + position directly; the pattern already ships for statics |
| Weather polish dropped (§10.7) | Item C4, low priority | Same framework, little extra plumbing |
| Offline ESRGAN rejected for creature sprites (§10.8) | Still rejected | Runtime kernel in `fpSprite` (B1) instead |

Day/night map blending stays out of scope: the engine holds one tileset at a
time.

## 5. Standing constraints

- Every new address or offset goes through `build_manifest`; unresolved →
  feature disabled, game untouched.
- Every item is INI-gated and A/B-toggleable at runtime.
- New hooks must coexist with EEex's detours (notably
  `CVidTile::RenderTexture` and `CInfinity::Render` screen-shake hooks).
- Validation is CI build + in-game check under Proton; host tests cover the
  parsing and packing logic.
