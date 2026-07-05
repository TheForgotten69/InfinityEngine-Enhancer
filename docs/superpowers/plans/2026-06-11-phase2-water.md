# Phase 2 — Water (Tile Shader, WED-Masked) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Animated water on exactly the WED-flagged liquid cells (not the whole screen), via a replacement `fpSEAM` override file driven by the proven uniform bridge and the unit-2 area mask texture.

**Architecture:** The packer gains a liquid-mode variant (texel = TileLiquidMode 0..5 per WED cell); `area_state` uploads it on texture unit 2 (already proven working). `features/tile_render` publishes scroll/zoom/world-size to the probe each tile draw; the probe feeds them as uniforms at program bind + frame tick (both proven). The replacement `fpSEAM.glsl` (game `override/` file — the validated delivery path) reproduces vanilla math when `uIeeEnabled < 0.5` and otherwise derives world position from `gl_FragCoord`, samples the mask, and applies a world-space water style only on liquid cells.

**Tech Stack:** C++20, CMake, GLSL 1.10 (ARB-era, no `#version` line — matches engine sources), MinHook (existing hooks only).

**Branch:** `feat/phase2-water` (already created and checked out).

**Standing rules:**
- Commits: plain messages, no Co-Authored-By trailer.
- Host tests run locally (`cmake --build cmake-build-debug --target iee_tests && ctest --test-dir cmake-build-debug --output-on-failure`). Windows-only files do not compile locally; CI compiles them when the PR opens (no per-push CI on feature branches anymore). Push via `git push https://github.com/TheForgotten69/InfinityEngine-Enhancer.git feat/phase2-water` (gh credential helper; SSH needs the user).
- In-game validation is the user's Windows session (Task 7 runbook).
- Proven facts to rely on (do not re-derive): uniform bridge works; engine loads GLSL from game `override/`; unit-2 R8 upload works at LoadArea; frame hook on gdi32!SwapBuffers; vanilla fpSEAM interface = `uTex, uTcScale, uColorTone, vTc, vRef, vColor`.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/iee/game/area_texture.h/.cpp` | modify | Add `pack_area_liquid_texture` (texel = liquid mode per cell) |
| `src/iee/area_state.h/.cpp` | modify | Upload liquid-mode texture; new `read_area_zoom`; publish world size |
| `src/iee/shader_probe.h/.cpp` | modify | `set_area_render_state`; new uniforms (scroll/zoom/viewport/worldSizeInv/areaMask) in the feed |
| `src/iee/features/tile_render.cpp` | modify | Publish scroll/zoom/world-size per tile draw |
| `assets/game-override/fpSEAM.glsl` | rewrite | Vanilla-faithful base + world-space, mask-gated water |
| `tests/iee_tests.cpp` | modify | Packer tests; shader-asset contract test |
| `docs/validation/phase0-gates.md` | modify | Phase 2 session runbook + verdicts |

---

### Task 1: Liquid-mode packer (host, TDD)

**Files:**
- Modify: `src/iee/game/area_texture.h`, `src/iee/game/area_texture.cpp`
- Test: `tests/iee_tests.cpp`

- [ ] **Step 1: Write the failing test** (append before `int main()`; register both in `main()` after `test_area_texture_packing_size_mismatch();`):

```cpp
void test_area_liquid_texture_packing() {
    iee::game::WedAreaInfo wed{};
    wed.baseWidth = 2;
    wed.baseHeight = 2;
    wed.overlays.resize(3);
    wed.overlays[1].liquidMode = iee::game::TileLiquidMode::Water;
    wed.overlays[2].liquidMode = iee::game::TileLiquidMode::Lava;
    // cell flags: bit N set = overlay N covers the cell
    wed.baseOverlayFlags = {
        0x00,        // no overlays -> mode 0
        0x02,        // overlay 1 (water) -> mode 1
        0x04,        // overlay 2 (lava) -> mode 2
        0x06,        // overlays 1+2 -> lowest overlay index wins -> mode 1
    };
    const auto packed = iee::game::pack_area_liquid_texture(wed);
    expect_true(packed.has_value(), "packs valid wed");
    if (packed) {
        expect_eq(packed->width, 2, "liquid width");
        expect_eq(packed->height, 2, "liquid height");
        expect_eq(packed->texels[0], std::uint8_t{0}, "no overlay -> 0");
        expect_eq(packed->texels[1], std::uint8_t{1}, "water overlay -> 1");
        expect_eq(packed->texels[2], std::uint8_t{2}, "lava overlay -> 2");
        expect_eq(packed->texels[3], std::uint8_t{1}, "first liquid overlay wins");
    }
}

void test_area_liquid_texture_packing_rejects_mismatch() {
    iee::game::WedAreaInfo wed{};
    wed.baseWidth = 3;
    wed.baseHeight = 1;
    wed.baseOverlayFlags = {0x00}; // wrong size
    expect_true(!iee::game::pack_area_liquid_texture(wed).has_value(),
                "flag/dimension mismatch -> nullopt");
    iee::game::WedAreaInfo empty{};
    expect_true(!iee::game::pack_area_liquid_texture(empty).has_value(), "empty -> nullopt");
}
```

- [ ] **Step 2: Build + run — expect FAIL** (`pack_area_liquid_texture` undeclared):

```bash
cmake --build cmake-build-debug --target iee_tests && ctest --test-dir cmake-build-debug --output-on-failure
```

- [ ] **Step 3: Implement.** `area_texture.h` — add below `pack_area_cell_texture`:

```cpp
    // Packs per-cell liquid classification: texel = TileLiquidMode (0..5) of the
    // lowest-index liquid overlay covering the cell, 0 when none. R8, row-major.
    [[nodiscard]] std::optional<AreaCellTexture> pack_area_liquid_texture(const WedAreaInfo &wed);
```

`area_texture.cpp` — add (include `"tile_liquid.h"` is already transitive via wed_runtime.h; add explicitly):

```cpp
    std::optional<AreaCellTexture> pack_area_liquid_texture(const WedAreaInfo &wed) {
        const std::size_t expected = std::size_t{wed.baseWidth} * wed.baseHeight;
        if (expected == 0 || wed.baseOverlayFlags.size() != expected) {
            return std::nullopt;
        }
        AreaCellTexture out{.width = wed.baseWidth, .height = wed.baseHeight};
        out.texels.resize(expected, 0);
        for (std::size_t cell = 0; cell < expected; ++cell) {
            const std::uint8_t flags = wed.baseOverlayFlags[cell];
            for (std::size_t overlay = 1; overlay < wed.overlays.size() && overlay <= 7; ++overlay) {
                if ((flags & (1u << overlay)) == 0) continue;
                const auto mode = wed.overlays[overlay].liquidMode;
                if (mode != TileLiquidMode::None) {
                    out.texels[cell] = static_cast<std::uint8_t>(mode);
                    break; // lowest overlay index wins
                }
            }
        }
        return out;
    }
```

- [ ] **Step 4: Build + run — expect PASS.**

- [ ] **Step 5: Commit** — `git add -A && git commit -m "game: pack per-cell liquid mode for the area mask texture"`

---

### Task 2: Zoom reader + liquid upload + world-size publish (Windows-only)

**Files:**
- Modify: `src/iee/area_state.h`, `src/iee/area_state.cpp`

- [ ] **Step 1: Add `read_area_zoom`** to `area_state.h`:

```cpp
    // Reads CInfinity::m_fZoom via the manifest offset (EEex docs +0x484).
    bool read_area_zoom(const game::CGameArea *area, const game::BuildManifest &manifest, float &outZoom);
```

and `area_state.cpp`:

```cpp
    bool read_area_zoom(const game::CGameArea *area, const game::BuildManifest &manifest, float &outZoom) {
        if (!area) {
            return false;
        }
        const auto *base = reinterpret_cast<const std::byte *>(area);
        const auto *zoomAddr = base + offsetof(game::CGameArea, m_cInfinity) + manifest.offsets.infinityZoom;
        return core::safe_read(zoomAddr, outZoom);
    }
```

(This finally consumes `infinityZoom` — added to the manifest in Phase 0 Task 3.)

- [ ] **Step 2: Switch the unit-2 upload to the liquid texture.** In `refresh_wed_cache`, replace `pack_area_cell_texture(*cachedWed)` with `pack_area_liquid_texture(*cachedWed)` and update the log text:

```cpp
        if (const auto packed = game::pack_area_liquid_texture(*cachedWed)) {
            ...
            LOG_INFO("Area liquid texture uploaded: {}x{} (unit 2, tex {})",
                     packed->width, packed->height, s_areaTexture);
```

(The raw-flags packer stays for tests/other consumers; only the upload switches.)

- [ ] **Step 3: Publish world size after a successful upload** (same block, after the LOG_INFO):

```cpp
            probe::set_area_world_size(static_cast<float>(packed->width) * 64.0f,
                                       static_cast<float>(packed->height) * 64.0f);
```

Add `#include "iee/shader_probe.h"` to `area_state.cpp`. (One WED base cell is 64 world pixels — the same constant `base_cell_index_from_screen_point` divides by.)

- [ ] **Step 4: Host tests still pass** (these files don't compile locally; run as sanity): `ctest --test-dir cmake-build-debug --output-on-failure`

- [ ] **Step 5: Commit** — `git add -A && git commit -m "area: upload liquid-mode mask and publish world size and zoom reader"`

---

### Task 3: Probe — area render state + new uniforms in the feed (Windows-only)

**Files:**
- Modify: `src/iee/shader_probe.h`, `src/iee/shader_probe.cpp`

- [ ] **Step 1: Header API** (`shader_probe.h`, after `override_effect_enabled`):

```cpp
    // Published by area_state (world size, at area load) and tile_render
    // (scroll/zoom, per tile draw); consumed by the uniform feed.
    void set_area_world_size(float widthPx, float heightPx) noexcept;
    void set_area_scroll_zoom(float scrollX, float scrollY, float zoom) noexcept;
```

- [ ] **Step 2: Module state** (`shader_probe.cpp`, next to `g_uniformTime`):

```cpp
        std::atomic<float> g_worldWidthPx{0.0f};
        std::atomic<float> g_worldHeightPx{0.0f};
        std::atomic<float> g_scrollX{0.0f};
        std::atomic<float> g_scrollY{0.0f};
        std::atomic<float> g_zoom{1.0f};
```

and the public setters (in the public API region):

```cpp
    void set_area_world_size(float widthPx, float heightPx) noexcept {
        g_worldWidthPx.store(widthPx, std::memory_order_relaxed);
        g_worldHeightPx.store(heightPx, std::memory_order_relaxed);
    }

    void set_area_scroll_zoom(float scrollX, float scrollY, float zoom) noexcept {
        g_scrollX.store(scrollX, std::memory_order_relaxed);
        g_scrollY.store(scrollY, std::memory_order_relaxed);
        g_zoom.store(zoom > 0.0f ? zoom : 1.0f, std::memory_order_relaxed);
    }
```

- [ ] **Step 3: Extend `UniformLocations`** (keep the existing four):

```cpp
        struct UniformLocations {
            int time{-2};    // -2 = not yet queried; -1 = queried, not found
            int enabled{-2};
            int liquidTime{-2};
            int liquidMode{-2};
            int scroll{-2};        // uIeeScroll (vec2, world px)
            int zoom{-2};          // uIeeZoom (float)
            int viewport{-2};      // uIeeViewport (vec2, physical px)
            int worldSizeInv{-2};  // uIeeWorldSizeInv (vec2, 1/world px)
            int areaMask{-2};      // uIeeAreaMask (sampler2D -> unit 2)
        };
```

- [ ] **Step 4: Extend `feed_uniforms_to_program`.** Add the lazy lookups:

```cpp
            if (locs.scroll == -2)       locs.scroll       = gl.glGetUniformLocation(program, "uIeeScroll");
            if (locs.zoom == -2)         locs.zoom         = gl.glGetUniformLocation(program, "uIeeZoom");
            if (locs.viewport == -2)     locs.viewport     = gl.glGetUniformLocation(program, "uIeeViewport");
            if (locs.worldSizeInv == -2) locs.worldSizeInv = gl.glGetUniformLocation(program, "uIeeWorldSizeInv");
            if (locs.areaMask == -2)     locs.areaMask     = gl.glGetUniformLocation(program, "uIeeAreaMask");
```

and after the existing value feeds:

```cpp
            if (locs.scroll >= 0 && gl.glUniform2f) {
                gl.glUniform2f(locs.scroll,
                               g_scrollX.load(std::memory_order_relaxed),
                               g_scrollY.load(std::memory_order_relaxed));
            }
            if (locs.zoom >= 0) {
                gl.glUniform1f(locs.zoom, g_zoom.load(std::memory_order_relaxed));
            }
            if (locs.viewport >= 0 && gl.glUniform2f && gl.glGetIntegerv) {
                int vp[4] = {0, 0, 0, 0};
                gl.glGetIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
                gl.glUniform2f(locs.viewport, static_cast<float>(vp[2]), static_cast<float>(vp[3]));
            }
            if (locs.worldSizeInv >= 0 && gl.glUniform2f) {
                const float w = g_worldWidthPx.load(std::memory_order_relaxed);
                const float h = g_worldHeightPx.load(std::memory_order_relaxed);
                if (w > 0.0f && h > 0.0f) {
                    gl.glUniform2f(locs.worldSizeInv, 1.0f / w, 1.0f / h);
                }
            }
            if (locs.areaMask >= 0 && gl.glUniform1i) {
                gl.glUniform1i(locs.areaMask, 2); // reserved unit (area_state upload)
            }
```

Note: `int vp[4]` — `GL_VIEWPORT` returns 4 ints; we feed width/height (vp[2], vp[3]).

- [ ] **Step 5: Host tests pass (sanity), commit** — `git add -A && git commit -m "probe: feed scroll, zoom, viewport, world size, and area mask sampler"`

---

### Task 4: tile_render publishes scroll/zoom per draw (Windows-only)

**Files:**
- Modify: `src/iee/features/tile_render.cpp`

- [ ] **Step 1: Replace the WED debug-log block.** Currently (in `render_tile`):

```cpp
        if (const auto wed = std::atomic_load(&ctx.wed)) {
            const auto *activeArea = ctx.activeArea.load();
            int offsetX = 0;
            int offsetY = 0;
            if (area::read_area_scroll(activeArea, offsetX, offsetY)) {
                std::uint8_t overlayFlags = 0;
                if (const auto cellIndex = game::base_cell_index_from_screen_point(*wed, x, y, offsetX, offsetY)) {
                    if (game::base_cell_has_liquid_overlay(*wed, *cellIndex, overlayFlags)) {
                        LOG_DEBUG_FAST("WED liquid coverage for cell {} flags=0x{:02X}", *cellIndex, overlayFlags);
                    }
                }
            }
        }
```

becomes:

```cpp
        if (std::atomic_load(&ctx.wed)) {
            const auto *activeArea = ctx.activeArea.load();
            int offsetX = 0;
            int offsetY = 0;
            if (area::read_area_scroll(activeArea, offsetX, offsetY)) {
                float zoom = 1.0f;
                (void) area::read_area_zoom(activeArea, *ctx.manifest, zoom);
                probe::set_area_scroll_zoom(static_cast<float>(offsetX),
                                            static_cast<float>(offsetY),
                                            zoom);
            }
        }
```

Add `#include "iee/shader_probe.h"` to the includes. (The per-cell CPU lookup goes away — classification now lives in the mask texture; the GPU does the lookup.)

- [ ] **Step 2: Host tests pass (sanity), commit** — `git add -A && git commit -m "tile_render: publish scroll and zoom to the uniform feed"`

---

### Task 5: The replacement fpSEAM (the shader itself)

**Files:**
- Rewrite: `assets/game-override/fpSEAM.glsl`
- Test: `tests/iee_tests.cpp`

- [ ] **Step 1: Write the failing contract test** (append + register in `main()`). It locks the asset to the engine interface and our feed names:

```cpp
void test_fpseam_override_asset_contract() {
    namespace fs = std::filesystem;
    const fs::path assetPath = fs::path("assets") / "game-override" / "fpSEAM.glsl";
    std::ifstream file(assetPath, std::ios::binary);
    expect_true(static_cast<bool>(file), "fpSEAM override asset exists (run tests from repo root)");
    if (!file) return;
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string source = contents.str();

    // Engine interface (from the live vanilla dump) must be fully preserved.
    constexpr std::string_view vanillaInterface =
        "uniform sampler2D uTex;\n"
        "uniform vec2 uTcScale;\n"
        "uniform vec4 uColorTone;\n"
        "varying vec2 vTc;\n"
        "varying vec2 vRef;\n"
        "varying vec4 vColor;\n";
    const auto contract = iee::game::check_interface_contract(vanillaInterface, source);
    expect_true(contract.ok, "fpSEAM override preserves the engine interface");
    for (const auto &missing : contract.missingIdentifiers) {
        std::cerr << "  missing identifier: " << missing << '\n';
    }

    // Our feed contract.
    for (const std::string_view name :
         {"uIeeEnabled", "uIeeTime", "uIeeScroll", "uIeeZoom",
          "uIeeViewport", "uIeeWorldSizeInv", "uIeeAreaMask"}) {
        expect_true(source.find(name) != std::string::npos,
                    "fpSEAM override declares feed uniform");
    }
    expect_true(source.find("#version") == std::string::npos,
                "no #version line (engine sources are ARB-era GLSL)");
}
```

Note: ctest runs with the build dir as CWD by default — set the test's working directory once in CMakeLists.txt if not already repo-root: in the `add_test` line, add `WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"`:

```cmake
    add_test(NAME iee_tests COMMAND iee_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
```

- [ ] **Step 2: Run — expect FAIL** (current asset is the legacy liquid patch: no uIeeScroll etc.).

- [ ] **Step 3: Write the shader.** Full content of `assets/game-override/fpSEAM.glsl`:

```glsl
#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

// fpSEAM.glsl
// IEE_V2_FPSEAM_WATER — world-space, WED-mask-gated water.
// uIeeEnabled < 0.5 reproduces the vanilla fpSEAM output exactly.

uniform lowp	sampler2D	uTex;
uniform highp 	vec2 		uTcScale;
uniform highp	vec4		uColorTone;

// --- IEE feed (set by the DLL at program bind + frame tick) ---
uniform highp	float		uIeeEnabled;       // 0/1 master gate (F10)
uniform highp	float		uIeeTime;          // seconds
uniform highp	vec2		uIeeScroll;        // world px of viewport origin
uniform highp	float		uIeeZoom;          // world->screen scale
uniform highp	vec2		uIeeViewport;      // physical px (w, h)
uniform highp	vec2		uIeeWorldSizeInv;  // 1 / world px
uniform lowp	sampler2D	uIeeAreaMask;      // unit 2: liquid mode per WED cell

varying highp	vec2		vTc;
varying highp	vec2		vRef;
varying lowp	vec4		vColor;

bool isBorderColor(vec4 testColor)
{
	if ((testColor.x == 0.0) && (testColor.y == 0.0) && (testColor.z == 0.0))
	{
		return true;
	}
	return false;
}

// Vanilla seam-aware sampling, parameterized on the sample coordinate so the
// water path can ride the same border handling with distorted coords.
vec4 seamSample(vec2 tc)
{
	vec2 texCoordUnbiased = tc / uTcScale;
	vec2 refCoordUnbiased = vRef / uTcScale;

	float fu = fract(texCoordUnbiased.x);
	float fv = fract(texCoordUnbiased.y);

	vec2 texCoordTileLoc = mod(texCoordUnbiased - refCoordUnbiased, 64.0);
	int texCoordTileLocIntx = int(floor(texCoordTileLoc.x));
	int texCoordTileLocInty = int(floor(texCoordTileLoc.y));

	vec4 texColor0 = texture2D(uTex, tc);

	vec4 texColor1 = texture2D(uTex, tc + vec2(uTcScale.x, 0.0));
	vec4 texColor2 = texture2D(uTex, tc + vec2(0.0, uTcScale.y));
	vec4 texColor3 = texture2D(uTex, tc + vec2(uTcScale.x, uTcScale.y));

	bool border0 = isBorderColor(texColor0);
	bool border1 = isBorderColor(texColor1);
	bool border2 = isBorderColor(texColor2);
	bool border3 = isBorderColor(texColor3);

	vec4 texColor = vec4(0.0, 0.0, 0.0, 1.0);

	if (((texCoordTileLocIntx == 0) || (texCoordTileLocIntx >= 63) ||
		 (texCoordTileLocInty == 0) || (texCoordTileLocInty >= 63)) &&
		 (border0 || border1 || border2 || border3) )
	{
		texColor = texColor0;
	}
	else
	{
		if (border1) { texColor1 = texColor0; }
		if (border2) { texColor2 = texColor0; }
		if (border3) { texColor3 = texColor0; }

		vec4 texColorTop = mix(texColor0, texColor1, fu);
		vec4 texColorBottom = mix(texColor2, texColor3, fu);
		texColor = mix(texColorTop, texColorBottom, fv);
	}

	return texColor;
}

float hash21(vec2 p)
{
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

void main()
{
	// World position of this fragment: gl_FragCoord is physical pixels with a
	// bottom-left origin; the engine's world is top-left, scaled by zoom.
	vec2 screenPx = vec2(gl_FragCoord.x, uIeeViewport.y - gl_FragCoord.y);
	vec2 worldPos = uIeeScroll + screenPx / max(uIeeZoom, 0.0001);

	// Liquid mode of the WED cell under this fragment (R8: 0..5 -> 0..5/255).
	float cellMode = 0.0;
	if (uIeeEnabled > 0.5)
	{
		cellMode = floor(texture2D(uIeeAreaMask, worldPos * uIeeWorldSizeInv).r * 255.0 + 0.5);
	}

	vec2 sampleTc = vTc;
	if (cellMode > 0.5)
	{
		// World-space wave field: continuous across tile and cell boundaries.
		float t = uIeeTime;
		vec2 w = worldPos * 0.085;
		vec2 distort;
		distort.x = sin(w.y * 1.7 + t * 1.1) + 0.5 * sin(w.x * 2.3 - t * 0.7);
		distort.y = cos(w.x * 1.9 - t * 0.9) + 0.5 * cos(w.y * 2.9 + t * 1.3);
		// Distortion in atlas texels, clamped well inside the 64px tile cell
		// so neighbor tiles never bleed in.
		sampleTc = vTc + distort * uTcScale * 1.5;
	}

	vec4 texColor = seamSample(sampleTc);

	if (cellMode > 0.5)
	{
		float t = uIeeTime;
		vec2 w = worldPos * 0.085;

		// Per-mode tint (TileLiquidMode: 1 water, 2 lava, 3 goo, 4 sewage, 5 swamp).
		vec3 deepTint = vec3(0.10, 0.22, 0.38);
		if (cellMode > 4.5)      { deepTint = vec3(0.16, 0.26, 0.10); } // swamp
		else if (cellMode > 3.5) { deepTint = vec3(0.30, 0.28, 0.08); } // sewage
		else if (cellMode > 2.5) { deepTint = vec3(0.12, 0.28, 0.08); } // goo
		else if (cellMode > 1.5) { deepTint = vec3(0.40, 0.12, 0.02); } // lava

		float swell = 0.5 + 0.5 * sin(w.x * 1.3 + w.y * 1.1 + t * 0.8);
		texColor.rgb = mix(texColor.rgb, texColor.rgb * (0.85 + 0.3 * swell) + deepTint * 0.12, 0.55);

		// Sparse moving glints.
		float glint = hash21(floor(worldPos * 0.25) + floor(t * 2.0));
		if (glint > 0.985)
		{
			texColor.rgb += vec3(0.25);
		}
	}

	texColor = texColor * vColor;

	float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
	vec3 tone = grey * uColorTone.rgb;

	gl_FragColor = vec4(mix(texColor.rgb, tone, uColorTone.a), texColor.a);
}
```

Design notes baked in: the disabled path is byte-for-byte vanilla math (`seamSample(vTc)` with `cellMode == 0`); waves are functions of `worldPos` (continuous across tiles, stable under scroll); distortion is ±1.5 texels (no neighbor-tile bleed past the seam handling); mask sampled with NEAREST (set at upload) so cell edges are hard — acceptable v1, smoothing is recorded polish.

- [ ] **Step 4: Run tests — expect PASS** (contract test now satisfied).

- [ ] **Step 5: Commit** — `git add -A && git commit -m "shaders: world-space WED-masked water fpSEAM override"`

---

### Task 6: Mask-alignment debug aid

**Files:**
- Modify: `assets/game-override/fpSEAM.glsl` (one block), `tests/iee_tests.cpp` (extend contract test)

The world-position derivation (y-flip, zoom, viewport) is the one unproven equation in this phase. Give the session a visual verifier.

- [ ] **Step 1:** In the shader, after the `cellMode` computation, add:

```glsl
	// Alignment debug: uIeeEnabled fed as 2.0 tints liquid cells red instead of
	// styling them — shoreline must hug the WED water edge exactly.
	if (uIeeEnabled > 1.5)
	{
		vec4 base = seamSample(vTc);
		base = base * vColor;
		if (cellMode > 0.5) { base.rgb = mix(base.rgb, vec3(1.0, 0.1, 0.1), 0.5); }
		float dbgGrey = dot(base.rgb, vec3(0.299, 0.587, 0.114));
		gl_FragColor = vec4(mix(base.rgb, dbgGrey * uColorTone.rgb, uColorTone.a), base.a);
		return;
	}
```

- [ ] **Step 2:** In `shader_probe.cpp`, make F10 a three-state cycle when debug hotkeys are on: OFF (0.0) -> ON (1.0) -> ALIGN (2.0) -> OFF. Replace the F10 handler body:

```cpp
        static bool f10WasDown = false;
        const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10Down && !f10WasDown) {
            const float current = g_overrideEffectValue.load(std::memory_order_relaxed);
            const float next = current < 0.5f ? 1.0f : (current < 1.5f ? 2.0f : 0.0f);
            g_overrideEffectValue.store(next, std::memory_order_relaxed);
            LOG_INFO("Hotkey F10: override effect value {}", next);
        }
        f10WasDown = f10Down;
```

This replaces `g_overridesEnabled` (bool) with `g_overrideEffectValue` (`std::atomic<float>`, default `0.0f`). Update: the declaration; `feed_uniforms_to_program` (`enabledValue = g_overrideEffectValue.load(...)`, and the legacy `liquidMode` feed becomes `enabledValue >= 0.5f ? 1.0f : 0.0f`); `set_override_effect_enabled(bool)` stores 1.0f/0.0f; `override_effect_enabled()` returns `>= 0.5f`. Grep for all `g_overridesEnabled` uses (`grep -nF g_overridesEnabled src/iee/shader_probe.cpp`) and convert every one.

- [ ] **Step 3:** Extend the contract test's feed-uniform loop — no new names needed (debug rides `uIeeEnabled`); just run tests — PASS.

- [ ] **Step 4: Commit** — `git add -A && git commit -m "probe+shader: F10 cycles off/on/alignment-debug"`

---

### Task 7: Validation runbook + docs

**Files:**
- Modify: `docs/validation/phase0-gates.md` (append Phase 2 section), `docs/architecture.md` (one line)

- [ ] **Step 1: Append to the gates doc:**

```markdown
## Phase 2 session — WED-masked water

Build: `<sha>`. Install the bundle DLL + copy `game-override/fpSEAM.glsl` into
the game `override/`. `EnableDebugHotkeys = true`. Use an area with water
(coast/river/harbor — check the log: `liquidOverlayMask` must be non-zero;
AR2300N was 0x00).

1. Load the area. Expect `Area liquid texture uploaded: WxH (unit 2, tex N)`
   and `Program 24 registered for uniform feed`.
2. F10 once (ON): water cells animate; land cells must be pixel-identical.
3. F10 again (ALIGN): liquid cells tint red. Walk the shoreline — the red
   region must hug the actual water edge. If offset: note the direction and
   whether zoom changes the offset (zoom in/out), screenshot, report. This
   diagnoses the gl_FragCoord -> world equation (y-flip / zoom / viewport).
4. F10 again (OFF): everything back to vanilla.
5. Scroll and zoom during ON: waves must stay glued to the world (no swimming
   against the scroll), continuous across tile boundaries.
6. Pause during ON: grey tone must still apply over the water.
7. Evidence: log + screenshots/clip per state.

Verdicts:
- Upload/registration: PASS/FAIL
- Alignment: PASS/FAIL (+offset notes)
- Water visuals: notes
- Regression (land tiles, fonts, sprites, pause): PASS/FAIL
```

- [ ] **Step 2:** `docs/architecture.md` — in the `src/iee/game/area_texture.*` line, append: "Packs both raw overlay flags and per-cell liquid modes; the liquid variant feeds the unit-2 mask texture."

- [ ] **Step 3: Commit + push** —

```bash
git add -A && git commit -m "docs: phase 2 water validation runbook"
git push https://github.com/TheForgotten69/InfinityEngine-Enhancer.git feat/phase2-water
```

- [ ] **Step 4: Hand off to the user** for the Windows session. STOP — alignment verdict gates any polish work.

---

## Out of scope (recorded, not planned)

- Mask edge smoothing (bilinear/dilated sampling at shorelines) — polish after alignment PASS.
- DuDv/foam texture upgrade (WIP-branch assets) — needs a texture-loading path for our own assets; separate task after v1 ships.
- fpSprite/FSR-EASU sprite work — Pillar 2, own plan.
- Removing the legacy `uIeeLiquidTime`/`uIeeTileLiquidMode` feed — keep until no patched-data installs remain in play.

## Self-Review Notes (plan time)

- Spec coverage: spec Phase 2 = replacement fpSEAM, world-space water, WED mask via Phase 0 area texture, night guard (water tint is additive-neutral, no luminance-based darkening — compliant), atlas-bleed clamp (±1.5 texels), emissives/AO correctly absent.
- The one acknowledged unknown is the world-position equation; Task 6 + runbook step 3 exist precisely to measure it, and fixes are uniform-side (no shader redesign).
- Type consistency: `pack_area_liquid_texture` returns `AreaCellTexture` (Task 1) consumed in Task 2; `set_area_world_size`/`set_area_scroll_zoom` declared Task 3, called Tasks 2/4; `g_overrideEffectValue` conversion is self-contained in Task 6.
```
