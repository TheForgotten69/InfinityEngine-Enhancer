# Sprite Upscale

## Problem Statement

The target runtime exposes editable GLSL shader files for sprite rendering. That makes character/monster sprite experiments a shader-file problem first, not a DLL-hook problem.

The v1 goal is narrow:

- Improve sprite rendering at normal gameplay zoom.
- Stay within the sprite shader family.
- Preserve engine-visible behavior such as alpha handling, outlines, and selection.

The v1 quality bar is not "FSR compiles". It is "the result looks clearly better than the current sprite-only Catmull-Rom plus light sharpening baseline".

## Scope

In scope:

- `fpsprite.glsl`
- `fpselect.glsl`
- Character, monster, corpse, and other content that the confirmed sprite programs already render

Out of scope:

- `fpseam.glsl`
- terrain and tile post-filtering
- UI shaders
- non-sprite map shaders
- generalized DLL-side shader interception beyond the confirmed sprite programs

## Baseline Inputs

Captured live runtime baselines are stored under:

- [`shaders/sprite/runtime_baseline/fpsprite.glsl`](../shaders/sprite/runtime_baseline/fpsprite.glsl)
- [`shaders/sprite/runtime_baseline/fpselect.glsl`](../shaders/sprite/runtime_baseline/fpselect.glsl)

Current extracted baseline observations:

- `fpsprite.glsl` declares `uTex`, `uSpriteBlurAmount`, `vTc`, and `vColor`.
- `fpselect.glsl` additionally declares `uTcScale`.
- The baseline `fpsprite.glsl` performs a blur-backed sprite composite rather than an upscale filter.
- The baseline `fpselect.glsl` implements outline detection in texel space.
- Ghidra-confirmed shader program mapping in `DrawInit_GL`:
  - slot `5` = `vpDraw` + `fpSprite`
  - slot `7` = `vpDraw` + `fpSELECT`
  - slot `8` = `vpDraw` + `fpSEAM`
- Ghidra-confirmed `DrawCreateProgram_GL` contract:
  - every GL program gets cached uniform locations for `uTcScale`, `uTex`, `uTex2`, `uColorTone`, `uZoomStrength`, `uTcClamp`, `uScreenHeight`, and `uSpriteBlurAmount`
  - the cache stride is `0x28`
  - the engine immediately seeds `uTex = 0` and `uTex2 = 1` on link
- Ghidra also confirms that shader compile failure is fatal during initialization, which makes startup breakage an immediate signal that a replacement shader is invalid.

Reference material:

- [`shaders/sprite/easu_rcas/`](../shaders/sprite/easu_rcas/) contains local FSR reference shaders.
- `dtiefling/dshaders` shows that sprite shader replacement, Catmull-Rom interpolation, and sprite-only sharpening are feasible through shader files alone.
- The libretro GLSL FSR conversion at `https://git.libretro.com/libretro-assets/glsl-shaders/-/tree/master/fsr/shaders` is another useful upstream reference, but it is not assumed to be ready for direct drop-in use in this game.

## Discovery Workflow

Before expanding scope, answer these questions from Ghidra plus live runtime behavior:

1. Which function loads `fpsprite.glsl` and `fpselect.glsl`.
2. Whether the engine pre-processes those files or compiles them as-is.
3. Which uniforms are bound for sprite shaders in practice.
4. Whether `uTcScale` is available to `fpsprite` replacement shaders.
5. Whether there is any sprite-local offscreen pass or intermediate render target.
6. How compile/link errors surface when a replacement shader fails.

The repo does not currently claim answers to those questions beyond the captured baseline files and external reference material.

If live shader behavior is ambiguous, enable DLL shader tracing with `EnableShaderTracing = true` in `InfinityEngine-Enhancer.ini`. The tracer logs shader source hashes and labels, compile/link results, interesting program binds, uniform existence, and runtime updates for `uTcScale` and `uSpriteBlurAmount`.

The tracer now also logs:

- interesting sprite draw calls through `glDrawArrays` and `glDrawElements`
- engine-side batch geometry through the resolved `DrawBegin`, `DrawBindTexture`, `DrawTexCoord`, `DrawVertex`, and `DrawEnd` wrappers
- framebuffer binds and framebuffer color attachment updates
- compact runtime summaries keyed by `{program, framebuffer, engineTexId}`
- raw GL texture summaries keyed by `{program, framebuffer, samplerUnit, texture}`
- automatic runtime-summary reset on each `LoadArea`
- an optional magenta batch-highlight probe keyed by one `{program, engineTexId}` pair
- an optional batch-suppress probe keyed by one `{program, engineTexId}` pair

Those logs are the current path to answer whether a sprite-only offscreen pipeline can be inserted cleanly.

Current runtime conclusion for BGEE `2.6.6.x`:

- the engine does load replacement `fpsprite.glsl` and `fpselect.glsl` files from disk
- `uTcScale` links successfully on both programs
- the engine leaves `uTcScale` at `0,0` unless the DLL populates it
- `fpselect.glsl` is primarily the selected-sprite outline path, not the main sprite-rendering path
- one-pass EASU-style shader experiments were near-no-op at gameplay zoom even after `uTcScale` injection
- startup program mapping alone is not sufficient to identify the live sprite body path during gameplay
- current gameplay traces show active draw traffic on `program 3`, `18`, and `24` on `framebuffer 0`, while startup-confirmed `fpsprite`/`fpselect` programs `15` and `21` do not reappear in the captured runtime window
- a loud `fpDraw` probe visibly affects actor bodies, which confirms that live actor-body rendering goes through the generic `fpDraw` path
- the same `fpDraw` probe also affects non-sprite scene content, so `fpDraw.glsl` is a good routing probe but a bad feature implementation point for sprite-only quality work
- raw GL program suppression produced the first reliable coarse role map:
  - `program 3` removes UI and in-world actors while leaving the map visible
  - `program 18` removes font/text
  - `program 24` removes map rendering while leaving actors and UI visible
- therefore the real actor body path is on raw GL `program 3`, but `program 3` is still too broad to use directly because it also carries UI
- raw GL texture probing on `program 3` now narrows that path further:
  - `texture 2` is the main character-sprite body path
  - `texture 1` is the selection/outline path
  - `texture 2` is still not perfectly sprite-exclusive because some world-map/icon content shares it
- the RenderTexture-resolved engine draw wrappers are not a sufficient interception point for actor-body work in this build
- this repo should treat sprite-FBO exploration as the next serious path, not more shader-file-only tuning

## Shader Contract Checklist

For sprite-only shader work, confirm:

- `uTex` remains the active sprite sampler.
- `vTc` stays normalized in sprite texture space.
- `uTcScale`, if available, behaves as one-texel UV size.
- `vColor` semantics are unchanged between baseline and replacement shaders.
- `fpselect.glsl` still receives the inputs needed for outline detection.

If any of those assumptions fails in live runtime testing, the shader-only path must be reconsidered before more code is added.

## Filter Decision Gate

The v1 filter choice is gated by the live shader contract:

- If there is only one sprite shader pass and no proven sprite-local intermediate target, use a one-pass shader only.
- If `uTcScale` or an equivalent texel-size input is available, a one-pass EASU-derived sampler is acceptable for v1.
- If the live contract does not expose stable texel-size data, reject the EASU path and fall back to a more appropriate one-pass filter.
- Do not add RCAS as a second pass unless an existing sprite-local intermediate and second programmable pass are proven from runtime evidence.

## Current Repo Implementation

The first repo-owned working copies live under:

- [`shaders/sprite/v1/fpsprite.glsl`](../shaders/sprite/v1/fpsprite.glsl)
- [`shaders/sprite/v1/fpselect.glsl`](../shaders/sprite/v1/fpselect.glsl)

The current design choices are:

- Use shader-file replacement for the actual sprite filter logic.
- Use targeted DLL assistance only to populate `uTcScale` for confirmed sprite programs from the bound texture dimensions.
- Treat `fpsprite` as the primary quality path and `fpselect` as selection-path correctness.
- Keep the EASU-derived reconstruction step conservative.
- Apply a stronger alpha-aware single-pass sharpen on solid sprite texels because runtime delta diagnostics showed the pure one-pass EASU path was too subtle to matter visually at gameplay zoom.
- Keep sprite alpha and selection-outline semantics conservative rather than chasing aggressive sharpening.
- Keep `fpselect` outline logic intact and only upscale solid sprite texels.
- Use the DLL tracer, not Ghidra alone, to answer live draw-boundary and framebuffer questions for a future sprite-only FBO design.
- Treat `fpDraw.glsl` as the confirmed live routing probe for actor bodies, but keep actual feature work out of raw `fpDraw` replacement because its scope is broader than sprites.
- Start the next iteration from [`shaders/sprite/v1/fpdraw.glsl`](../shaders/sprite/v1/fpdraw.glsl), with the shader body gated by `uIeeSpriteBodyMode` so only raw `{program 3, texture 2}` draws take the sprite-body filter path.

Diagnostic variants are also provided under `shaders/sprite/v1/diagnostics/` to answer two runtime questions quickly:

- Is the game actually loading the replacement sprite shader file.
- Is `uTcScale` non-zero inside `fpsprite` and `fpselect`.

This is intentionally not a claim that full FSR is integrated. It is a gated one-pass sprite experiment.

The repo now also contains enough tracing to pivot to a sprite-FBO prototype once the engine’s draw-boundary and framebuffer behavior are confirmed from logs.

Current recommendation after the `fpDraw` routing probe:

- do not ship a global `fpDraw` FSR pass
- do not keep spending time on `fpsprite`/`fpselect` as if they were the main body path
- do not use RenderTexture-resolved engine wrappers as the primary actor interception point for this build; they miss the in-world actor body path
- move the real implementation toward DLL-side filtering on raw GL `program 3`, with additional discrimination to exclude UI before any upscale or sharpen filter is applied
- use `uIeeSpriteBodyMode` as the first concrete implementation seam: the DLL decides when `{program 3, texture 2}` is active, and `fpDraw.glsl` only applies sprite-body filtering for those draws

Targeted runtime confirmation:

- when a candidate `{program, engineTexId}` pair looks actor-like but is not yet proven, enable the batch-highlight probe and set `BatchHighlightProgram` plus `BatchHighlightTextureId`
- the probe forces those batches to render in magenta through the engine draw-color wrapper and restores the previous color at `DrawEnd`
- use it as a routing proof only; it is not a feature path
- if the magenta probe logs matches but nothing visible changes, enable the batch-suppress probe for the same pair
- suppression is the stronger proof: if the actor disappears, the pair is correct and the color override path is simply ineffective for those textured draws
- if texture IDs are still too broad, add screen-size bounds to the suppress probe and target actor-sized batches directly
- if those still hit UI or portraits, add center-position bounds so the suppress probe only targets in-world batches near the actor
- if the RenderTexture-resolved engine wrappers still do not affect the in-world sprite, pivot to raw `glDrawArrays`/`glDrawElements` suppression keyed by the active program
- raw GL suppression is now proven to be the correct coarse layer for role discovery:
  - `program 3` is the mixed actor/UI path
  - `program 18` is text
  - `program 24` is map/background
- the next discriminator therefore has to split actor draws from UI draws within raw `program 3`
- the preferred next probe is `glTextureSummary` on raw `program 3`, because brute-forcing wrapper-side engine texture ids is no longer the right search space
- once candidate raw GL textures are known, use the raw GL texture suppress probe on `program 3` before trying any feature work on that path
- current validated texture split for BGEE `2.6.6.x`:
  - `{program 3, texture 2}` = main character sprite body path
  - `{program 3, texture 1}` = outline/select path

## Acceptance Criteria

Build a fixed screenshot set at normal gameplay zoom and compare:

- vanilla live shader
- Catmull-Rom baseline
- Catmull-Rom plus light sharpening baseline
- repo v1 shader

Required improvements:

- cleaner perceived sprite edges
- less blockiness
- less shimmer in movement
- no selection-outline regressions
- no transparent-edge halos
- no obvious ringing or oversharpen artifacts

If the v1 shader does not beat the Catmull-Rom baseline, keep the docs and baseline captures, but do not force the result under an "FSR" label.

## Risks And Fallback Policy

- `fpsprite.glsl` replacement shaders can declare and link `uTcScale`, but the engine does not populate it on its own for this build.
- One-pass reconstruction can improve sampling but cannot invent missing detail from low-quality sprite art.
- Transparent sprite edges are the highest-risk area for halos and black fringe artifacts.
- If the replacement shader does not compile or the injected texel scale is unavailable for the currently bound sprite texture, fall back to the baseline shader source rather than widening the runtime hook scope.
