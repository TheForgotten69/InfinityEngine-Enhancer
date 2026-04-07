# Reverse Engineering Notes

## Workflow Rules

- Record RVAs, not absolute runtime VAs.
- Runtime VA = module base + RVA.
- Ghidra, PDB, and in-game logs can disagree on absolute addresses because of ASLR and function chunking.
- Treat EEex-documented layouts as authoritative when they match runtime behavior.
- Treat build-manifest offsets as locally validated runtime facts that may need revalidation per build.
- Shader-name assumptions are not enough for sprite work. Treat live shader file contents plus runtime evidence as authoritative for shader behavior.

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

## Sprite Shader Investigation Targets

For sprite-upscale v1, the reverse-engineering targets are the live `fpsprite.glsl` and `fpselect.glsl` files and the code path that loads them.

Questions to answer with Ghidra and runtime evidence:

- Which loader function maps shader filenames to compiled shader/program objects.
- Whether `fpsprite.glsl` and `fpselect.glsl` are loaded as standalone files or with preprocessing/includes.
- Which uniforms are bound for sprite shaders, especially whether `uTcScale` is available to `fpsprite` replacements.
- Whether sprites ever pass through an existing sprite-local intermediate render target or second programmable pass.
- How shader compile/link failures surface at runtime.

Current local evidence for the target runtime:

- Extracted baseline `fpsprite.glsl` declares `uTex`, `uSpriteBlurAmount`, `vTc`, and `vColor`.
- Extracted baseline `fpselect.glsl` declares `uTex`, `uSpriteBlurAmount`, `uTcScale`, `vTc`, and `vColor`.
- Ghidra review of `DrawCreateProgram_GL` confirms the per-program uniform cache layout with stride `0x28`:
  - `+0x00` program object
  - `+0x04` `uST`
  - `+0x08` `uTcScale`
  - `+0x0C` `uTex`
  - `+0x10` `uTex2`
  - `+0x14` `uColorTone`
  - `+0x18` `uZoomStrength`
  - `+0x1C` `uTcClamp`
  - `+0x20` `uScreenHeight`
  - `+0x24` `uSpriteBlurAmount`
- The same `DrawCreateProgram_GL` path immediately binds the program and seeds `uTex = 0` and `uTex2 = 1` when present. This makes the cached offsets above the right static-search targets for live setter callsites.
- Runtime tracing now confirms that replacement `fpsprite.glsl` and `fpselect.glsl` files are compiled from disk for this build.
- Runtime tracing also confirms that both sprite programs link `uTcScale`, but the engine leaves the live value at `0,0` unless the DLL injects texel scale from the bound texture.
- `fpselect.glsl` behaves as the selected-sprite outline path; `fpsprite.glsl` remains the primary sprite-rendering path worth tuning for quality.
- Ghidra review of `DrawInit_GL` confirms the current shader program mapping:
  - program `5` = `vpDraw` + `fpSprite`
  - program `7` = `vpDraw` + `fpSELECT`
  - program `8` = `vpDraw` + `fpSEAM`
- The same `DrawInit_GL` path treats shader compile failure as fatal and shows a message box before exiting, which confirms that direct shader-file replacement is part of the normal startup path.
- Inference from the same function: there is also a separate `fpCatRom` postprocess program behind `gl.pp.enabled`, but this is not evidence of a sprite-local two-pass path.
- Ghidra review of `DrawHookUpGLFunctions` confirms the runtime resolves:
  - shader lifecycle calls such as `glCreateProgram`, `glCreateShader`, `glShaderSource`, `glLinkProgram`, and `glUseProgram`
  - uniform setters including `glUniform1f`, `glUniform2fv`, and related ARB variants
  - framebuffer functions including `glGenFramebuffers`, `glBindFramebuffer`, and `glFramebufferTexture2D`

## Current Runtime Trace Workflow

With `EnableShaderTracing = true`, the DLL tracer now logs:

- shader source hashes, labels, and compile/link results
- uniform existence and current values for `uTcScale`, `uTex`, and `uSpriteBlurAmount`
- sprite draw calls via `glDrawArrays` and `glDrawElements` for interesting programs
- engine-side batch geometry through the resolved `DrawBegin`, `DrawBindTexture`, `DrawTexCoord`, `DrawVertex`, and `DrawEnd` wrappers
- framebuffer binds and color attachment updates around interesting programs
- compact runtime summaries keyed by `{program, framebuffer, engineTexId}`
- raw GL texture summaries keyed by `{program, framebuffer, samplerUnit, texture}`
- an automatic runtime-summary reset on each `LoadArea`

That makes the DLL the preferred source of truth for:

- whether `fpSprite` or `fpSELECT` is active for a given scene
- whether the engine is writing texel-scale or blur uniforms at draw time
- whether sprite draws already pass through a reusable offscreen framebuffer path

The next static Ghidra targets should therefore be xrefs to the cached `gl.programs` offsets rather than raw uniform strings:

- `+0x00` for program selection and `glUseProgram`
- `+0x08` for `uTcScale`
- `+0x24` for `uSpriteBlurAmount`

Current live-trace gap:

- startup `DrawInit_GL` mapping proves slot `5 = fpSprite` and `7 = fpSELECT`, but current gameplay traces are dominated by runtime `program 3`, `18`, and `24`
- there is no observed gameplay framebuffer transition away from `framebuffer 0` in the captured scene
- until the live actor body path is identified from the summarized runtime trace, do not assume the startup slot mapping is the active sprite-rendering path

Current live-trace conclusion:

- a loud `fpDraw` shader probe visibly affects actor bodies, which confirms that the live actor-body path runs through the generic `fpDraw` family rather than through startup `fpSprite`/`fpSELECT` alone
- the same probe also affects non-sprite scene content, so `fpDraw` is a valid routing probe but not a safe sprite-only implementation point
- RenderTexture-resolved engine draw-batch hooks are not sufficient to isolate the in-world actor body path in this build
- when runtime summaries narrow the search to a small `{program, engineTexId}` set, use the batch-highlight probe to tint exactly one pair magenta and verify whether it is the actor body path
- if a matched batch never changes color on screen, use the batch-suppress probe on the same pair to distinguish “wrong pair” from “color override ineffective”
- when one texture ID still covers too much scene content, use the batch suppress screen-size bounds to target actor-like quads instead of the whole texture bucket
- when actor-like size buckets still hit bottom-screen portraits or UI, add center-position bounds so the probe only suppresses in-world batches around the actor
- if the RenderTexture-resolved engine draw wrappers never affect the in-world actor, use a raw GL program suppress probe instead; the `fpDraw` shader replacement already proves that raw GL layer does see the actor
- raw GL program suppress results for BGEE `2.6.6.x`:
  - `program 3` removes UI and in-world actors while leaving the map visible
  - `program 18` removes font/text
  - `program 24` removes map rendering while leaving actors and UI visible
- therefore raw GL `program 3` is the first confirmed coarse entrypoint for actor-body work, but it is too broad on its own because it also carries UI
- the next useful discriminator must separate in-world actor draws from UI draws within raw GL `program 3`, rather than searching for a different top-level shader program
- shader filename classification in the tracer must be case-insensitive, otherwise `fpDraw.glsl`/`fpFont.glsl`/`fpSEAM.glsl` can collapse to `fragment-other` and lose the uniform metadata needed for texture-level draw summaries
- the next live proof step after `glTextureSummary` is raw GL texture suppression keyed by `{program 3, texture}`, which is the narrowest currently proven probe that still sees actor bodies
- current validated raw GL texture split for BGEE `2.6.6.x`:
  - `{program 3, texture 2}` suppresses the main in-world character sprite body path
  - `{program 3, texture 1}` suppresses the selection/outline path
  - `{program 3, texture 2}` still has some collateral non-character use, notably shared icon-like content outside the core actor path
- the first concrete implementation seam on top of that split is a shader uniform gate:
  - `uIeeSpriteBodyMode = 1` only for raw `{program 3, texture 2}`
  - `uIeeSpriteBodyMode = 0` for every other `fpDraw` draw
  - this allows `fpDraw.glsl` to carry sprite-body-only filter logic without changing the rest of the `fpDraw` path

Until a sprite-local intermediate is proven, treat full two-pass FSR as unsupported and keep sprite-upscale work in the one-pass shader-replacement bucket.
