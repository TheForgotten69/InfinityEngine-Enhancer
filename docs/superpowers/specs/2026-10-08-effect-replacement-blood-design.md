# Shader effect replacement, with blood as the first effect

Date: 2026-10-08
Status: Design, awaiting owner review. Nothing here is built.
Target: BGEE 2.7.3 (x64, Proton), EEex-loaded DLL. Other builds leave it off.

Roadmap item C2 of
[2026-10-03-graphics-uplift-roadmap-design.md](2026-10-03-graphics-uplift-roadmap-design.md).
The standing constraints of that document apply (manifest-only addresses,
fail closed, INI gate plus runtime toggle, coexist with EEex's detours).

## 1. Goal

A mechanism that lets a GLSL shader repaint a chosen engine effect, and one
effect that proves it: the blood splash on a hit.

Owner intent (2026-10-08): the remaining effect work is "replace effects with
GLSL"; start with blood, then add effects one at a time, most common first.
Anything not replaced must keep drawing exactly as today.

Success for this spec:

- Hit blood is drawn as smooth, glossy liquid at screen resolution instead of
  magnified sprite pixels, in the creature's own blood colour, darkened by the
  area light like everything else, hidden behind walls like today.
- A debug hotkey switches between the engine's blood and ours while playing.
- Adding the next effect needs a table entry and a shader, not new plumbing.
- The log lists which effects were replaced and which other effect art was
  seen at the same hook, so the next effects are chosen from play, not guessed.

## 2. Evidence (2.7.3, decompiled 2026-10-08)

### 2.1 "Blood" is two systems

`CGameSprite::AddBlood` (`0x347BC0`) branches on the blood type:

| Kind | Path | Drawn by |
|---|---|---|
| Hit blood (`GUSH_LOW/MEDIUM/HIGH`, `PULSATING_ARTERY`) | `StartSpriteEffect(0, type, 0)` (`0x3794D0`) loads `BLOODS`, `BLOODM`, `BLOODL` or `BLOODCR` into the creature's `m_spriteSplashVidCell` (`CGameSprite + 0x3DE0`) and recolours its palette with the creature's blood colour (`GetColorBlood` / `m_bloodColor`) | `CGameSprite::RenderSpriteEffect` (`0x370E20`), called from `CGameSprite::Render` |
| Exploding death (`EXPLODING_DEATH`) | A `CBlood` object (`0x220F00`) holding `CParticle` dots and blobs, kept in `m_lstBlood` | `CBlood::Render` (`0x224F20`) through `CParticle::Render`, before or after the body depending on direction |

Hit blood is the common case and the trial. The particle system is a later
second user of the same shader (section 9).

### 2.2 The hit-blood draw

`RenderSpriteEffect` draws the splash cell while its sequence has not ended:
`FXPrep` / `FXLock` / `FXRender` composite the frame on the CPU into the sprite
atlas, `FXRenderClippingPolys` cuts it against the wall polygons, and
`FXUnlock` / `FXBltFrom` queue one quad. The position is the creature's
`m_pos` plus its height offset; `MIRROR_FX` flips it for mirrored animations.

The same function and the same cell also draw the fire, shock and acid impact
art (`SPFIRIMP`, `SPSHKIMP`, `SPBURN`, `SPSPARKS`, or a VVC named by the
effect). Whether a call is blood is decided by the splash cell's resref, never
by the call.

The four BAMs are small and short: 6 to 13 frames, 5 to about 50 pixels on a
side (`BLOODS` 8 frames up to 17x16, `BLOODM` 13 up to 23x43, `BLOODL` 13 up
to 49x52, `BLOODCR` 6 up to 47x45). At the owner's zoom (about 7.5 screen
pixels per world pixel) one splash covers up to roughly 370 screen pixels.

### 2.3 The draw queue merges draws

`DrawEnd_GL` (`0x42B220`) appends a command only when the state word differs
from the previous command's; otherwise it adds the new vertices to the
previous command's `primCount`. One command can therefore hold several
sprites. A draw is identified by its vertex range, not by a command index:
`gl.user.primStart` (`0x2F73F70`, `0x60` bytes before `gl.n`) is the vertex
cursor, and it advances by exactly the vertices a draw adds.

`DrawFlush_GL` resets the cursor to 0. The mod already calls the engine's
flush more than once per frame with a rewritten queue (the bloom's emissive
replay), and vertex data and atlas textures stay valid across those calls
until new draws are queued.

## 3. Approach: the engine's own draw becomes the guide

The engine keeps drawing the effect. The mod records which vertices that draw
added, and when the queue is flushed it sends just those vertices to an
offscreen "guide" image instead of the screen, then runs the effect's shader
over that part of the screen with the guide as input.

Per replaced draw, inside one flush:

1. Flush everything queued before it to the screen (the engine's flush, on a
   shortened queue).
2. Clear the guide image in the effect's screen rectangle, bind it, and let
   the engine's flush draw only the effect's vertices into it.
3. Bind the screen again and draw the effect shader over that rectangle,
   reading the guide.
4. Continue with the rest of the queue.

Why this shape:

- **Everything the engine decides stays decided by the engine**: position,
  mirroring, the creature's blood colour, the area light tint, wall clipping,
  draw order against other creatures, the gore option, and movement smoothing
  (the hook runs inside the already-smoothed creature render). None of it has
  to be re-derived or kept in step.
- **Logic is untouched.** The engine's draw still runs, so frame counters,
  durations and the cells `InterpolateAnimations` counts for a creature are
  exactly as today.
- **Off means off.** With the feature disabled or failed, no range is
  recorded and the flush is the engine's own.

What it costs: the shader can only paint where the engine's art is, plus a
small margin. It gives a better-looking version of the same splash, not a
different, larger or longer one.

### 3.1 Alternatives considered

| Alternative | Why not now |
|---|---|
| **Free-form draw**: skip the engine's draw, flush, and draw our own quad with our own simulation (droplets with their own motion, any size, any lifetime) | Flashier, but it loses wall clipping (blood shows through walls), and position, mirroring, colour, light tint and the gore option all have to be rebuilt by hand. Skipping the engine's draw also has to be proven not to change effect lifetime. Worth revisiting per effect if the guided result is too tame; the table (section 4.2) leaves room for it |
| **Paint in the tile shader**, as fire and smoke do today | Sits under every creature, is multiplied by the tile light, and costs a per-pixel loop over all points. Those limits are why item C wanted to leave `fpSEAM` |
| **Batch all effects before the fog** | One pass instead of one per effect, but every effect is drawn over creatures standing in front of it |
| **Replace the BAM art** with better pre-rendered frames | Still sprite pixels magnified 7x; the owner rejected every sprite upscaler |

## 4. Components

New code lives in its own files; `world_post.cpp` is already 1,100 lines.

### 4.1 `game/effect_ranges.*` (pure, unit-tested)

Tracks vertex ranges and plans a flush.

- `EffectRangeRecorder`: `begin(effect, cursor)` / `end(cursor)` around a
  hooked draw; holds up to 16 ranges per flush. `on_flush()` returns the ranges
  and clears them. A range still open at a flush (the engine flushes mid-frame
  when its atlas fills) is closed at the current cursor and reopened at 0.
  An empty range (the engine drew nothing) is dropped.
- `plan_flush(commands, ranges)`: turns the queue into an ordered list of
  segments, each a run of commands tagged "screen" or "effect N". A command
  whose vertices straddle a range boundary is split into copies with adjusted
  `primStart` / `primCount`. Clear commands (`primCount == -1`) stay in screen
  segments. Anything implausible (overlapping ranges, counts out of bounds)
  yields a single screen segment: the engine's own frame.

### 4.2 `game/effect_table.*` (pure, unit-tested)

Maps an effect resref to how it is replaced: a shader kind and its
parameters. Entries for this spec: `BLOODS`, `BLOODM`, `BLOODL`, `BLOODCR` to
the `Liquid` kind. The table is compiled in; no file format until a second
effect kind shows what one would need.

### 4.3 `features/world_effects.*` (GL)

Owns the guide image (screen-sized RGBA8, recreated on resize) and the effect
programs. Three calls used by the flush detour: clear the guide in a
rectangle and bind it; bind the screen back; draw an effect over a rectangle.
Uses `GlPassGuard` / `GlStateGuard` as the world post passes do, and restores
the framebuffer bindings it found.

The small GL helpers both features need (`Target`, `create_target`,
`build_program`, the full-screen vertex array) move from `world_post.cpp` into
a shared `features/gl_pass_util.*`. No behaviour change; nothing else in
`world_post.cpp` is restructured.

### 4.4 Hook and flush changes (`hooks.cpp`, `build_manifest`)

- New manifest pattern for `CGameSprite::RenderSpriteEffect`, with a
  `static_assert` pinning it to the function's first bytes and a log line for
  the resolved address (the 2026-10-08 lesson). New layout value: the vertex
  cursor's distance from `gl.n`.
- The detour reads the splash cell's resref. If the world pass is open, the
  feature is on and the table has the resref, it records the cursor, calls the
  engine, and records the cursor again. Otherwise it only calls the engine.
  The effect's screen rectangle comes from the creature position and the
  cell's current frame box, padded by the shader's reach.
- `detour_draw_flush` takes the recorded ranges. With none, it behaves as
  today. With some, it runs the segments of `plan_flush` through the engine's
  flush one after another (the first carries the atlas upload), switching
  target around effect segments, then does the emissive replay and the atlas
  restore as today.

## 5. The liquid shader

Input: the guide image, which holds the engine's splash as premultiplied
colour (drawn with `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` into a cleared target, so
its alpha is the sprite's alpha squared; the shader takes the square root).

1. **Shape.** Sample the guide's alpha around the pixel with a small kernel
   whose radius is a fraction of a world pixel times the zoom, and threshold
   the result with a soft edge. Blurring then thresholding rounds the
   magnified pixel blocks into smooth blobs and joins neighbouring droplets.
2. **Colour.** The blurred colour divided by the blurred alpha: the
   creature's blood colour with the engine's light tint already in it.
3. **Depth and gloss.** The gradient of the blurred alpha stands in for a
   surface normal: a darker rim, a slightly lighter centre and a small
   highlight, so it reads as wet rather than flat.
4. **Output.** Normal alpha blending onto the screen. Blood is not additive
   and does not feed the bloom.

Shaders are validated offline with `glslangValidator`, like the others.

## 6. Configuration

```ini
[Effects]
; Draw hit blood as smooth, glossy liquid instead of sprite pixels.
ShaderBlood = false
; How wet it looks: 0 = flat colour, 1 = strong rim and highlight.
BloodGloss = 0.6
```

Default off in the sample; on, with a visibly strong gloss, in the owner's
INI. With `EnableDebugHotkeys`, one key toggles it; F7 to F10 are taken, so
the key is chosen after checking the game's own key bindings.

## 7. Failure handling and logging

- Any GL error creating the guide or a program, or an invalid manifest:
  the feature turns itself off with one log line and the engine draws as
  before.
- More than 16 replaced draws in one flush: the extra ones are drawn by the
  engine as today.
- Logged once per resref: "replaced" or the reason it was not. Logged once
  per other resref seen in the splash cell: this is the list of impact art
  the hook could replace next.
- The per-area `World post:` line gains the number of replaced draws and
  the number of extra flushes that frame, so the cost is visible.

## 8. Testing and validation

- Unit tests: the range recorder (nesting refused, flush while open, empty
  ranges, the cap) and `plan_flush` (no ranges, a range inside one merged
  command, a range across several commands, a range at either end, clear
  commands, implausible input); the table lookup.
- Host tests and the mingw compile check locally, then CI.
- In game, by the owner: hit something, toggle the key, compare. Checks worth
  making: blood colour on a non-red creature, a hit behind a wall, a hit on a
  mirrored creature, a crowded fight (cost), and that fire and shock impacts
  look unchanged.

## 9. Known limits

- The splash keeps the original's shape, size, duration and frame steps. This
  is a better-looking version of the same effect.
- Each replaced draw adds two calls to the engine's flush and two small
  passes. Fine for a handful per frame; the log shows the count.
- Overlapping semi-transparent pixels within one splash accumulate in the
  guide differently from on screen; with single-layer splash frames this does
  not arise.
- 2.7.3 only.

## 10. Out of scope, in likely order

1. Exploding-death blood: bracket `CBlood::Render` the same way so its dots
   and blobs feed the same liquid shader and merge into streams.
2. The other impacts drawn by the same hook (fire, shock, acid).
3. Spell effects: `CVEFVidCell::Render` is the next hook site.
4. Blood that stays on the ground for a while. That is a new effect, not a
   replacement, and needs its own place in the draw order.
5. A free-form effect kind (section 3.1), if a specific effect needs it.
