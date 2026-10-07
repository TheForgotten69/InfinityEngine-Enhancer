# Soft Fog of War and Bloom — Design

Date: 2026-10-07. Roadmap items D1 and D2 of
`2026-10-03-graphics-uplift-roadmap-design.md`. Supersedes the fog and bloom
phases (4 and 5) of `2026-06-10-graphics-enhancement-roadmap-design.md`, whose
bracket design rests on a premise this document disproves (§2.1).

## 1. Goal

Two world-view effects with a visible result on every map:

- **Soft fog of war.** The engine's fog edge is built from 32 px linear
  gradient pieces and reads as diamonds and stair-steps. Replace it with a
  smooth edge of the same extent and darkness.
- **Bloom.** Bright parts of the world (fires, spell effects, lit windows,
  lava) get a soft glow.

Owner decisions (2026-10-07):

- Bloom is threshold-based now, tunable. A light-source contribution is added
  later, with effect-cast light (roadmap C3), not in this work.
- Animation in-betweening (old phase 1) is paused; this work comes first.

Constraints carried over: INI-gated and off by default, UI untouched, 2.7.3
only, every address through `build_manifest`, any failure leaves the game
drawing exactly as vanilla.

Done means: the owner confirms both effects in game under Proton, CI is green,
and with both settings off the frame is the engine's own.

## 2. Evidence (BGEE 2.7.3, decompiled 2026-10-07)

### 2.1 The engine queues the whole frame

`DrawEnd_GL` (`0x42B220`) does not draw. It appends a command (state word,
first vertex, count) to `gl.cmds`. The GPU sees nothing until `DrawFlush_GL`
(`0x42B350`, static, no arguments) walks the queue, applies state changes
against its cached `gl.hwState`, and issues `glDrawArrays`.

`DrawFlush_GL` is called from `DrawFlip_GL` (once per frame, before
`SDL_GL_SwapWindow`), from `DrawReadPixels_GL` and `DrawLockSurface_GL`, and
from `DrawBeginScaled_GL` / `DrawEndScaled_GL` only when `gl.pp.enabled`.

Consequences:

- Binding a framebuffer around `CInfinity::RenderFog` captures nothing. The
  2026-06-10 fog design ("bind an FBO, call the original, blur") cannot work
  as written.
- Any GL draw of ours issued during the world pass lands **under** the
  still-queued world, unless the queue is flushed first.
- A mid-frame flush is an operation the engine performs itself (screenshots,
  surface locks), so calling it at a chosen point has precedent.

### 2.2 Flush side effects

After the draws, `DrawFlush_GL` sets `gl.hwState` to the last command's
state, clears `gl.force`, resets the vertex cursor, and rewrites `gl.v.z` and
`gl.depthLockState`. Checked against a full disassembly (2026-10-07):

- `gl.v.z` (`0x142F73F7C`) is written only by `DrawInit_GL` and
  `DrawFlush_GL`, always to the same value, and read only by `DrawVertex_GL`.
  It never changes during a session.
- `gl.depthLockState` (`0x142F74048`) is written by the same two functions
  and never read.
- `DrawLockSurface_GL` (`0x42CAF0`) calls `DrawFlush_GL` whenever the
  engine's 1024x1024 sprite atlas fills, so flushes in the middle of the world
  pass already occur in vanilla on busy scenes.

An extra flush between the world and the fog therefore changes no state that
later draws depend on, provided our own GL state is restored (§5).

### 2.3 Engine post-processing is off in this build

`DrawInit_GL` reads `Graphics/Postprocessing` and then sets
`gl.pp.enabled = false` unconditionally. The engine's own offscreen target
(`gl.pp.fb`, blitted with `fpCatRom`) is therefore never used in 2.7.3, which
matches gate V5 (no engine framebuffer binds) and V3 (`fpCatRom` dead). Our
code still saves and restores whatever framebuffer is bound rather than
assuming 0.

### 2.4 Fog draw

`CInfinity::RenderFog` (`0x2A1B60`) is the last world step before
`CInfinity::PostRender` (weather) in `CGameArea::Render` (`0x189360`). It
pushes draw state, disables texturing, sets `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`,
and emits triangles for every visible 64 px tile through
`CVisibilityMap::BltFogOWar3d` (never to be detoured) inside one
`DrawBegin`/`DrawEnd`. The fog colour is `0x030103` after fade and
brightness, i.e. near black; only alpha varies. `RenderFog` is outside the
`DrawBeginSort`/`DrawEndSort` range, which is confined to `CInfinity::Render`.

## 3. Approach

One hook on `CInfinity::RenderFog`, plus the address of `DrawFlush_GL`:

```
detour RenderFog:
  flush                       # world so far is now in the framebuffer
  if bloom:  copy world -> glow chain -> add glow to framebuffer
  if fog:    bind fog target, clear white
             original RenderFog
             flush             # fog triangles land in the fog target
             rebind engine framebuffer
             blur fog target -> multiply framebuffer by it
  else:      original RenderFog
  restore GL state
```

Weather and the UI are queued afterwards and drawn at flip, on top, as today.
Bloom runs before fog so unexplored areas stay dark.

### 3.1 Why white-clear and multiply

Drawing alpha-blended black over a white target leaves, per pixel, the
product of `(1 - alpha)` over every fog triangle covering it: exactly the
fraction of the scene the engine's fog would let through, correct for
overlapping triangles too. Multiplying the framebuffer by that value
reproduces the engine's result (the fog colour's own contribution, at most
3/255, is dropped). With blur radius 0 the output must match vanilla to
within that error; this is the acceptance check for the capture path.

### 3.2 Alternatives considered

| Alternative | Why not now |
|---|---|
| Skip the original `RenderFog`; build a small visibility texture from the tile codes and draw one smooth-upsampled pass | No second flush and full control of the falloff, but it re-implements the engine's fog rules (explored vs unexplored shades, corner codes, fade, the fog option) from `CVisibilityMap` internals and needs several more manifest entries. Worth revisiting if the blurred capture looks wrong at the edges |
| Redirect the whole world pass into our own framebuffer and blit at the end | Gives a clean world-only image but moves every engine draw off the default framebuffer; far larger blast radius for the same result |
| Do the bloom at flip time | The UI is already in the image by then and would glow |
| Capture into a transparent target | Blending onto an empty target squares alpha and compounds on overlap; not recoverable in general |

## 4. Components

| Unit | Responsibility | Depends on |
|---|---|---|
| `game/world_post_plan.*` | Pure sizing and parameter logic: target sizes, glow chain levels, fog blur radius in pixels from world radius and zoom, clamping of INI values. Host-tested | nothing |
| `core/gl_pass_guard.h` | RAII save/restore of every GL state our passes touch (§5) | GL function table |
| `features/world_post.*` | Owns GL resources (targets, programs, quad). `begin_world_post()` / `end_fog_capture()` style entry points called by the hook. Lazy creation, resize on viewport change, drop on context change, one-shot failure latch | plan, guard, GL table |
| `hooks.cpp` | `detour_render_fog`, install/uninstall, gating | manifest, config |
| `game/build_manifest.*` | Patterns and reference RVAs for `CInfinity::RenderFog` and `DrawFlush_GL`, each verified unique offline | — |
| `game/opengl_types.*` | Added entries: framebuffer objects, `glBlitFramebuffer`, vertex array and buffer objects, and the fixed-function calls the guard needs | — |
| `core/config.*` | New INI keys (§6) | — |

Shaders are short GLSL sources embedded in `world_post.cpp` (our own
programs, not engine overrides): copy/downsample, bright-pass, blur
up/down, composite.

### 4.1 Bloom pass

1. `glBlitFramebuffer` the framebuffer to a half-resolution RGBA16F target
   (linear filter).
2. Bright-pass with a soft knee around `BloomThreshold`.
3. Dual-filter blur: downsample through a fixed number of levels (count from
   the plan, capped by target size), then upsample back, accumulating.
4. Add the result to the framebuffer (`ONE, ONE`) scaled by `BloomStrength`.

At the hook point the framebuffer holds only world content for this frame, so
no world rectangle is needed and the UI cannot glow.

### 4.2 Fog pass

1. Full-size RGBA8 fog target, cleared white; original `RenderFog`; flush.
2. Downsample to quarter size, blur with the same dual-filter code; radius
   from `SoftFogRadius` (world pixels) times the current zoom, taken from the
   view transform the mod already publishes (`area_state.h`).
3. Multiply the framebuffer by the blurred result (`DST_COLOR, ZERO`), with a
   small ordered dither to hide 8-bit banding in the long dark gradient.

Radius 0 skips step 2 and composites the capture directly.

## 5. GL state contract

The engine believes the hardware state equals `gl.hwState` after a flush and
only issues changes relative to it. Everything our passes change must be put
back exactly. The rule is "restore everything our passes change", which is:

- `GlPassGuard`: current program, vertex array and array buffer bindings,
  viewport, blend enable and functions, scissor, depth, stencil, alpha-test
  and cull-face enables, clear colour.
- `GlStateGuard({0})`: active texture and the 2D binding on unit 0. Our
  passes only ever bind textures on unit 0.
- The feature module itself: the draw and read framebuffer bindings, read at
  the hook and rebound before returning.

Not touched, so not saved: depth and colour masks (depth test is off, so
nothing writes depth), client-active texture, unit 1, texture environment.

All of this runs on the render thread inside an engine render call, the same
thread and context as the existing draw-time hooks.

## 6. Configuration

`[Rendering]`, all off or neutral by default:

| Key | Default | Meaning |
|---|---|---|
| `SoftFogOfWar` | `false` | Enable the fog capture and soft composite |
| `SoftFogRadius` | `24` | Blur radius in world pixels; `0` = capture without blur (must match vanilla) |
| `Bloom` | `false` | Enable bloom |
| `BloomThreshold` | `0.80` | Brightness above which pixels glow (0..1) |
| `BloomStrength` | `0.35` | Glow intensity |

With `EnableDebugHotkeys` on, one key per effect toggles it at runtime for
A/B comparison, using the existing hotkey mechanism.

## 7. Failure handling and logging

- Missing or non-unique pattern for either address: both features off, one
  log line naming which.
- Missing GL entry point, incomplete framebuffer, shader compile or link
  error: log the reason once, latch the feature off for the session, call the
  original `RenderFog` untouched.
- Context change: drop all GL objects and recreate on next use.
- Field log, once per area load: which effects engaged, target sizes, zoom,
  the framebuffer found bound at the hook, and one line the first time each
  pass draws. Verbose logging adds per-frame counts.

## 8. Build order and validation

Each step is a CI bundle the owner can check in game.

1. **Flush and capture, no blur.** Hook, manifest entries, guard, fog capture
   with `SoftFogRadius = 0`. Check: looks identical to `SoftFogOfWar = false`,
   including sprite/wall occlusion, weather, UI and screenshots. This
   validates the capture path and the state restore of §5.
2. **Soft fog.** Blur and dither. Check: edge smoothness at several zoom
   levels, no light leak at explored boundaries, radius tuning by eye.
3. **Bloom.** Check: fires and spells glow, UI does not, bright daytime maps
   are not hazy at defaults; tune threshold and strength by eye.

Host tests cover `world_post_plan` (sizes, level counts, radius scaling,
clamping) and INI parsing. GL behaviour is validated in game only.

## 9. Known limits and risks

- **GL state restore** (§5): a missed piece of state corrupts the engine's next draws; step 1 of §8 is the check.
- Selection circles, health bars and other bright world-space markers are
  part of the world image and will glow slightly. The later light-source
  path is the fix.
- Weather (rain, snow, lightning flash) is drawn after the hook and does not
  glow.
- The soft edge reveals a few pixels more at the visible boundary and darkens
  a few pixels inside it; the radius controls both.
- Threshold bloom on painted maps can haze bright scenes; defaults are
  conservative and the owner tunes by eye.
- 2.7.3 only. Other builds leave both features off.

## 10. Out of scope

Light-source bloom, effect-cast light, normal-derived relief, FXAA, water
reflections, changes to the engine's fog rules or colours.

## 11. Revision 2026-10-07: bloom from the engine's additive draws

In-game result of the threshold bloom (owner): bright painted art such as
paper glowed as much as flames. The threshold approach is removed.

`CVidCell::RenderTexture` (`0x425530`) selects the blend mode from the sprite
render flags: flag `0x8` or `0x200` gives a destination factor of `GL_ONE`,
everything else is normal alpha blending. The engine's own art flags therefore
mark what emits light. Every queued command keeps its blend factors in its
state word (`DrawBlendFunc_GL`: source at bits 9-12, destination at 13-16;
factor table at `0x5C12B0`, index 1 = `GL_ONE`).

Design:

- `DrawFlush_GL` is detoured. During the world pass (frame boundary to
  `RenderFog`) the detour copies the queue's additive commands, calls the
  original, then binds a black "emissive" target, writes those commands back
  into the queue and calls the original again. Vertex data and atlas textures
  from the first flush are still valid: the engine overwrites them only when
  new draws are queued.
- `gl.n` is located from the rip-relative compare at `DrawFlush_GL+0x24`;
  `gl.cmds` lies `0x180A8` bytes before it (`BuildManifest::drawQueue`).
- Before the fog, the emissive image is halved, blurred through the bloom
  chain, and applied twice: `framebuffer += framebuffer * light * LightSpill`
  (surfaces near a source brighten and take its colour) and
  `framebuffer += light * BloomStrength` (the halo).
- INI: `BloomThreshold` is gone; `LightSpill` (default 1.5, 0 = off, max 8).

Known limits: the replay has no depth buffer, so a light source hidden behind
a wall still glows; the light spill is screen-space, with no falloff model and
no wall occlusion; art drawn with flag `0x200` (`DST_COLOR, ONE`) contributes
nothing to a black target.

## 12. Revision 2026-10-07 (later): fog drift and light falloff

Owner feedback on section 11's build: bloom and fog both "feel better"; asked
for more.

- **Fog drift.** The blurred fog is composited by its own program, which
  offsets the lookup by slow two-octave value noise anchored to world
  coordinates (`SoftFogDrift`, world pixels, default 10, 0 = still). Flat
  regions are unaffected, so only the edge moves. Needs the view transform;
  without it the edge stays still.
- **Light falloff.** The light spill now samples a deeper level of the bloom
  chain (wide blurs only) and maps it through `1 - exp(-x)`, so a small source
  lights a wider area and the area next to it saturates instead of clipping.

## 13. Revision 2026-10-07 (later): fog smoothed over time

Owner observation: the fog is "pinned to one tick". The engine recomputes
visibility on its logic tick, so the reveal steps while sprites and the camera
move at display rate.

The blurred fog is now blended with the image shown on the previous frame
(`SoftFogSmoothing`, seconds, default 0.25, 0 = off): each frame the shown fog
moves `1 - exp(-dt / smoothing)` of the way to the new capture
(`game::temporal_blend`). The previous image is sampled at the same world
position, using the current and previous view transforms, so scrolling and
zooming do not smear it; positions that were off screen take the new value.
History is dropped on area load, on resize, after a gap of more than 0.5 s,
and whenever the view transform is unknown. The unblurred path
(`SoftFogRadius = 0`) is not smoothed and still reproduces the engine's fog.

This relies on `area::read_view_transform` being correct at `RenderFog` time,
which is not yet confirmed in game: a wrong scroll would show as fog trailing
behind the camera.

## 14. Revision 2026-10-07 (later): heat shimmer

Owner verdict on section 13: fog smoothing is "perfect". Next pick: heat haze.
Rejected by the owner: smoothing day/night transitions (areas swap maps, so
there is nothing to blend) and radius-based light sources with wall blocking
(not convinced it would work).

`HeatShimmer` (world pixels, default 2.5, 0 = off, needs `Bloom`): after the
emissive blur and before the glow is added, the world image is copied and
redrawn with its lookup rippled by world-anchored, upward-flowing noise. The
ripple is scaled by how warm the blurred emissive image is (red minus blue),
read at the pixel and at two points below it so the haze sits above its
source. Cold and white effects do not shimmer.

All shaders are checked offline with `glslangValidator` (extract the
`R"glsl(...)glsl"` bodies from `world_post.cpp`; append the shared noise
source to those that declare `float noise(vec2 p);`). A shader that fails to
compile in game switches fog and bloom off for the session, so run this
before pushing shader changes.

## 15. Revision 2026-10-07 (later): what else is stepped on the logic tick

Owner request: find everything pinned to the engine tick and decouple it.
Survey of the world-pass draws against their per-tick updates (2.7.3):

| Thing | Per-tick update | Status |
|---|---|---|
| Creatures, circles, health bars, 8 projectile/effect types | `m_pos` | Smoothed earlier (item A) |
| Fog of war | visibility map | Smoothed (section 13) |
| Rain, snow, sparkles | `CParticle::AsynchronousUpdate` (`0x423C30`) adds velocity to position | **Smoothed now** |
| Floating text over creatures | `CGameText::AIUpdate` copies the target's `m_pos` | **Smoothed now** |
| Selection-circle pulse, talk markers | `CMarker::AsynchronousUpdate` counters | Not done: a size animation, not a position; low value |
| `CProjectileSkyStrikeBAM`, spell-hit and travel-door projectiles | not drawn from `m_pos` | Not done |
| Animation frames of effects and ambient animations | frame counters | Needs in-between art (paused item F) |
| Screen fades, day/night tint | stepped values | Not done; day/night rejected by the owner |
| Camera scroll | — | Already at display rate under EEex |

Particles: `CParticle::Render` (`0x425BE0`; called by `CVidMode::BKRender`
for rain and snow and by `CSparkleCluster::Render`) is wrapped. A `TickClock`
fed from the particle update gives the fraction of the current tick elapsed;
the particle is drawn at `position - velocity * (1 - fraction)`, which is
exact because the update is a plain add. Gravity-only particles step back by
their gravity in height. The position is restored after the engine call
unless the engine changed it meanwhile. Layout mirrored as `game::CParticle`.

Floating text: `CGameText::Render` (`0x1F39A0`) joins the smoothed-object
table, so it slides with its creature.

Both ride on `SmoothSpriteMovement`. Known gap: the tick clock is global, so
if some particles keep updating while others are frozen (time stop during
rain), the frozen ones jitter by one step.
