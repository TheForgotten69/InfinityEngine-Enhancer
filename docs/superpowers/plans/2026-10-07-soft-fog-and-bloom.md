# Soft Fog of War and Bloom Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an INI-gated soft fog-of-war edge and a threshold bloom to the BGEE 2.7.3 world view, leaving the UI and every other build untouched.

**Architecture:** One detour on `CInfinity::RenderFog` calls the engine's own queue flush (`DrawFlush_GL`) so the finished world is in the framebuffer, runs bloom on it, lets the engine draw its fog into our white-cleared target, flushes again, then multiplies the framebuffer by the blurred fog. Pure sizing logic is host-tested; GL work lives in one feature module behind a state guard.

**Tech Stack:** C++20, CMake, MinHook (existing `core::Hook`), OpenGL 4.6 compatibility context through the existing `game::gl::OpenGLFunctions` table, GLSL 3.30, the repo's single-file test executable `iee_tests`.

**Spec:** `docs/superpowers/specs/2026-10-07-soft-fog-and-bloom-design.md`

## Global Constraints

- BGEE 2.7.3 only. Other builds leave both features off (manifest entries empty).
- Every new address goes through `build_manifest` as pattern + reference RVA. Never an ad-hoc constant.
- All settings default off/neutral: `SoftFogOfWar=false`, `SoftFogRadius=24`, `Bloom=false`, `BloomThreshold=0.80`, `BloomStrength=0.35`.
- Any failure (unresolved pattern, missing GL entry point, incomplete framebuffer, shader error) logs once, latches the feature off, and calls the original `RenderFog` untouched.
- Never detour `CVisibilityMap::BltFogOWar3d`.
- With both settings off the detour must not flush: the frame is the engine's own.
- Restore every GL state our passes touch; the engine caches hardware state in `gl.hwState`.
- No new dependencies. No AI attribution in commits or PRs.
- Windows-only code (`windows.h`, GL table, MinHook) stays out of `iee_common` and the host tests.
- CI uses GCC 13 with `-Werror`; build with `-DIEE_WARNINGS_AS_ERRORS=ON` locally.

## Review Focus

1. **Window resize or resolution change mid-session.** Targets must be rebuilt at the new size; no stretched or cropped fog. Pinned by the extent-change path in Task 6 (in-game check) and `test_world_post_plan` extent cases in Task 2.
2. **Zoom at both extremes.** Fog radius must scale with zoom and never request more blur levels than the target can hold. Pinned by `blur_plan_for_radius` tests in Task 2 and the two-zoom log check in Task 7.
3. **Nothing to capture.** A fully explored area, or fog switched off in the game options, queues no fog triangles; the result must be the plain scene, not a black or white screen. Pinned by the Task 6 in-game checklist (white target multiplies by 1).
4. **Mid-frame engine flush from elsewhere.** A screenshot (`DrawReadPixels`) must still capture a correct frame with the effects on. Pinned by the Task 6 in-game checklist.
5. **Garbage INI values.** `SoftFogRadius = nan`, negative strength, threshold above 1 must clamp to safe values. Pinned by `test_config_world_post` in Task 3.

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/iee/game/world_post_plan.h/.cpp` | new | Pure sizing: extents, blur level count, radius-to-plan, zoom scale. In `iee_common`, host-tested |
| `src/iee/core/config.h/.cpp` | modify | Five new `[Rendering]` keys, clamped in `normalize` |
| `src/iee/game/build_manifest.h/.cpp` | modify | `renderFog` and `drawFlush` patterns + reference RVAs |
| `src/iee/game/game_addrs.h/.cpp` | modify | Resolve the two addresses when a world-post setting is on |
| `src/iee/game/opengl_types.h/.cpp` | modify | Framebuffer, vertex-array, buffer and fixed-function entry points; `postProcessAvailable` |
| `src/iee/core/gl_pass_guard.h` | new | RAII save/restore of the non-texture state our passes touch |
| `src/iee/features/world_post.h/.cpp` | new | GL resources and the passes; the only file that draws |
| `src/iee/hooks.cpp` | modify | `detour_render_fog`, install/uninstall, area-load notification |
| `CMakeLists.txt` | modify | New sources in `iee_common` and the DLL |
| `tests/iee_tests.cpp` | modify | Plan, config and manifest tests |
| `docs/…` , `../handoff.md` | modify | Evidence, status, runtime facts |

Build commands used throughout (run from `InfinityEngine-Enhancer/`):

```bash
# Host tests (CI flags)
cmake -S . -B build-host -DBUILD_TESTING=ON -DIEE_WARNINGS_AS_ERRORS=ON -DIEE_ENABLE_SANITIZERS=ON
cmake --build build-host --target iee_tests && ctest --test-dir build-host --output-on-failure
# Windows compile check
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake -DIEE_BUILD_WINDOWS_DLL=ON -DIEE_WARNINGS_AS_ERRORS=ON
cmake --build build-mingw
```

Shell quirks on the owner's machine: `ls` is aliased (use `command ls`), zsh `noclobber` is on (use `>|`), unmatched globs are errors.

Work on a new branch `feature/world-post` created from `feature/are-animation-detection` (the hooks this builds on live there).

---

### Task 1: Depth-state question — resolved before implementation (no work)

Spec §2.2 raised the risk that an extra `DrawFlush_GL` disturbs depth ordering. It was settled offline on 2026-10-07 and recorded in the spec; nothing here needs doing. Kept as a numbered entry so later task numbers match the spec's references.

Finding (2.7.3, whole-binary disassembly searched for every reference):

- `gl.v.z` (`0x142F73F7C`) is written only by `DrawInit_GL` and `DrawFlush_GL`, always to the same value, and read only by `DrawVertex_GL`. It is constant for the whole session.
- `gl.depthLockState` (`0x142F74048`) is written by the same two functions and never read.
- `DrawLockSurface_GL` (`0x42CAF0`) calls `DrawFlush_GL` whenever the engine's 1024x1024 sprite atlas fills, so flushes in the middle of the world pass already happen in vanilla on busy scenes.

Verdict: **SAFE**. Call `DrawFlush_GL` directly; no engine globals need saving.

---

### Task 2: Sizing logic (`world_post_plan`)

**Files:**
- Create: `src/iee/game/world_post_plan.h`, `src/iee/game/world_post_plan.cpp`
- Modify: `CMakeLists.txt` (the `iee_common` source list, after `src/iee/game/wed_runtime.cpp`)
- Test: `tests/iee_tests.cpp`

**Interfaces:**
- Produces (namespace `iee::game`):
  - `struct Extent { int width{}; int height{}; }` with `operator==`
  - `inline constexpr int kMaxBlurLevels = 6;`
  - `struct BlurPlan { int levels{}; float offset{1.0f}; };`
  - `Extent half_extent(Extent) noexcept;`
  - `int available_blur_levels(Extent base) noexcept;`
  - `BlurPlan blur_plan_for_radius(float radiusPixels, Extent base) noexcept;`
  - `int bloom_levels(Extent base) noexcept;`
  - `float pixels_per_world_pixel(float viewportWidth, float viewWorldWidth) noexcept;`

A blur chain built on a `base` image has level `i` at `base` halved `i + 1` times. `levels == 0` means "do not blur".

- [ ] **Step 1: Write the failing test**

Add the include next to the other `iee/game` includes in `tests/iee_tests.cpp`:

```cpp
#include "iee/game/world_post_plan.h"
```

Add before `int main()`:

```cpp
void test_world_post_plan() {
  using namespace iee::game;

  expect_true(half_extent({1920, 1080}) == Extent{960, 540}, "Half extent halves both sides");
  expect_true(half_extent({1, 1}) == Extent{1, 1}, "Half extent never reaches zero");
  expect_true(half_extent({0, 0}) == Extent{1, 1}, "A degenerate extent still yields 1x1");

  expect_eq(available_blur_levels({1920, 1080}), 6, "A full-HD image supports the level cap");
  expect_eq(available_blur_levels({64, 64}), 3, "Levels stop before a side drops below 8");
  expect_eq(available_blur_levels({10, 10}), 0, "A tiny image supports no blur levels");
  expect_eq(available_blur_levels({0, 0}), 0, "A degenerate image supports no blur levels");

  const auto none = blur_plan_for_radius(0.0f, {1920, 1080});
  expect_eq(none.levels, 0, "Radius 0 means no blur");
  expect_eq(blur_plan_for_radius(std::numeric_limits<float>::quiet_NaN(), {1920, 1080}).levels, 0,
            "A NaN radius means no blur");
  expect_eq(blur_plan_for_radius(-5.0f, {1920, 1080}).levels, 0, "A negative radius means no blur");

  const auto small = blur_plan_for_radius(4.0f, {1920, 1080});
  expect_eq(small.levels, 1, "A 4 px radius needs one level");
  expect_eq(small.offset, 1.0f, "A radius equal to the level reach uses offset 1");

  const auto typical = blur_plan_for_radius(24.0f, {1920, 1080});
  expect_eq(typical.levels, 4, "A 24 px radius needs four levels (reach 32)");
  expect_eq(typical.offset, 0.75f, "Offset scales the reach down to the radius");

  const auto huge = blur_plan_for_radius(1000.0f, {1920, 1080});
  expect_eq(huge.levels, 6, "Levels are capped by what the image supports");
  expect_eq(huge.offset, 1.5f, "Offset is clamped so taps never skip texels badly");

  expect_eq(blur_plan_for_radius(24.0f, {10, 10}).levels, 0,
            "No blur is planned on an image too small to hold a level");

  expect_eq(bloom_levels({960, 540}), 5, "Bloom uses at most five levels");
  expect_eq(bloom_levels({40, 40}), 2, "Bloom levels shrink with the image");

  expect_eq(pixels_per_world_pixel(3840.0f, 1920.0f), 2.0f, "Scale is viewport over world width");
  expect_eq(pixels_per_world_pixel(3840.0f, 0.0f), 1.0f, "An unknown world width falls back to 1");
  expect_eq(pixels_per_world_pixel(0.0f, 1920.0f), 1.0f, "An unknown viewport falls back to 1");
  expect_eq(pixels_per_world_pixel(100000.0f, 1.0f), 8.0f, "Scale is clamped to a sane maximum");
  expect_eq(pixels_per_world_pixel(std::numeric_limits<float>::infinity(), 100.0f), 1.0f,
            "A non-finite input falls back to 1");
}
```

Call it in `main()` after `test_animation_interpolation();`:

```cpp
  test_world_post_plan();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-host --target iee_tests`
Expected: compile error, `iee/game/world_post_plan.h: No such file or directory`.

- [ ] **Step 3: Write the header**

`src/iee/game/world_post_plan.h`:

```cpp
#pragma once

namespace iee::game {
// Sizing for the world post-processing passes (soft fog of war, bloom). Pure
// arithmetic so it can be tested without a GL context.

struct Extent {
  int width{};
  int height{};
  friend bool operator==(const Extent&, const Extent&) = default;
};

inline constexpr int kMaxBlurLevels = 6;

// A dual-filter blur: `levels` successive half-size images below the source,
// sampled with `offset` texels of spread. levels == 0 means "do not blur".
struct BlurPlan {
  int levels{};
  float offset{1.0f};
};

Extent half_extent(Extent extent) noexcept;
// How many half-size levels fit below `base` before a side drops under 8 px.
int available_blur_levels(Extent base) noexcept;
// The plan whose blur reaches about `radiusPixels` on an image of `base` size.
BlurPlan blur_plan_for_radius(float radiusPixels, Extent base) noexcept;
int bloom_levels(Extent base) noexcept;
// Screen pixels per world pixel at the current zoom; 1 when either is unknown.
float pixels_per_world_pixel(float viewportWidth, float viewWorldWidth) noexcept;
}  // namespace iee::game
```

- [ ] **Step 4: Write the implementation**

`src/iee/game/world_post_plan.cpp`:

```cpp
#include "iee/game/world_post_plan.h"

#include <algorithm>
#include <cmath>

namespace iee::game {
namespace {
constexpr int kMinBlurExtent = 8;
constexpr int kMaxBloomLevels = 5;
}  // namespace

Extent half_extent(Extent extent) noexcept {
  return {std::max(1, extent.width / 2), std::max(1, extent.height / 2)};
}

int available_blur_levels(Extent base) noexcept {
  int levels = 0;
  Extent extent = base;
  while (levels < kMaxBlurLevels) {
    extent = half_extent(extent);
    if (std::min(extent.width, extent.height) < kMinBlurExtent) break;
    ++levels;
  }
  return levels;
}

BlurPlan blur_plan_for_radius(float radiusPixels, Extent base) noexcept {
  // Written so NaN fails the test.
  if (!(radiusPixels >= 1.0f)) return {};
  const int available = available_blur_levels(base);
  if (available == 0) return {};
  int levels = 1;
  while (levels < available && static_cast<float>(1 << (levels + 1)) < radiusPixels) ++levels;
  const auto reach = static_cast<float>(1 << (levels + 1));
  return {levels, std::clamp(radiusPixels / reach, 0.5f, 1.5f)};
}

int bloom_levels(Extent base) noexcept {
  return std::min(kMaxBloomLevels, available_blur_levels(base));
}

float pixels_per_world_pixel(float viewportWidth, float viewWorldWidth) noexcept {
  if (!std::isfinite(viewportWidth) || !std::isfinite(viewWorldWidth)) return 1.0f;
  if (!(viewportWidth > 0.0f) || !(viewWorldWidth > 0.0f)) return 1.0f;
  return std::clamp(viewportWidth / viewWorldWidth, 0.25f, 8.0f);
}
}  // namespace iee::game
```

Add to `CMakeLists.txt` in the `iee_common` list, after `src/iee/game/wed_runtime.cpp`:

```cmake
    src/iee/game/world_post_plan.cpp
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake -S . -B build-host -DBUILD_TESTING=ON -DIEE_WARNINGS_AS_ERRORS=ON -DIEE_ENABLE_SANITIZERS=ON && cmake --build build-host --target iee_tests && ctest --test-dir build-host --output-on-failure`
Expected: `All InfinityEngine-Enhancer native tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/iee/game/world_post_plan.h src/iee/game/world_post_plan.cpp CMakeLists.txt tests/iee_tests.cpp
git commit -m "World post: sizing logic for blur chains and zoom scale"
```

---

### Task 3: Configuration keys

**Files:**
- Modify: `src/iee/core/config.h` (struct `EngineConfig`), `src/iee/core/config.cpp` (`normalize`, `apply_kv` `[Rendering]` block, the save function's `Rendering` section)
- Test: `tests/iee_tests.cpp`

**Interfaces:**
- Produces: `EngineConfig::softFogOfWar` (bool, false), `softFogRadius` (float, 24), `bloom` (bool, false), `bloomThreshold` (float, 0.80), `bloomStrength` (float, 0.35). After `ConfigManager::load` the floats are finite and within `[0,256]`, `[0,1]`, `[0,2]`.

- [ ] **Step 1: Write the failing test**

Add before `int main()` in `tests/iee_tests.cpp`:

```cpp
void test_config_world_post() {
  iee::core::EngineConfig defaults{};
  expect_true(!defaults.softFogOfWar && !defaults.bloom, "World post effects default off");
  expect_eq(defaults.softFogRadius, 24.0f, "Soft fog radius default");
  expect_eq(defaults.bloomThreshold, 0.80f, "Bloom threshold default");
  expect_eq(defaults.bloomStrength, 0.35f, "Bloom strength default");

  const auto tempPath =
      std::filesystem::current_path() / "InfinityEngine-Enhancer-world-post-test.ini";
  {
    std::ofstream out(tempPath, std::ios::trunc);
    out << "[Rendering]\n";
    out << "SoftFogOfWar = true\n";
    out << "SoftFogRadius = 40\n";
    out << "Bloom = true\n";
    out << "BloomThreshold = 0.6\n";
    out << "BloomStrength = 0.5\n";
  }
  iee::core::EngineConfig cfg{};
  expect_true(iee::core::ConfigManager::load(tempPath, cfg), "World post keys should load");
  expect_true(cfg.softFogOfWar && cfg.bloom, "World post bools should parse");
  expect_eq(cfg.softFogRadius, 40.0f, "Soft fog radius should parse");
  expect_eq(cfg.bloomThreshold, 0.6f, "Bloom threshold should parse");
  expect_eq(cfg.bloomStrength, 0.5f, "Bloom strength should parse");

  {
    std::ofstream out(tempPath, std::ios::trunc);
    out << "[Rendering]\n";
    out << "SoftFogRadius = 100000\n";
    out << "BloomThreshold = 7\n";
    out << "BloomStrength = -3\n";
  }
  cfg = {};
  expect_true(iee::core::ConfigManager::load(tempPath, cfg), "Out-of-range values should load");
  expect_eq(cfg.softFogRadius, 256.0f, "Soft fog radius is clamped to its maximum");
  expect_eq(cfg.bloomThreshold, 1.0f, "Bloom threshold is clamped to 1");
  expect_eq(cfg.bloomStrength, 0.0f, "Bloom strength is clamped to 0");

  {
    std::ofstream out(tempPath, std::ios::trunc);
    out << "[Rendering]\n";
    out << "SoftFogRadius = nan\n";
    out << "BloomThreshold = inf\n";
    out << "BloomStrength = nan\n";
  }
  cfg = {};
  expect_true(iee::core::ConfigManager::load(tempPath, cfg), "Non-finite values should load");
  expect_eq(cfg.softFogRadius, 24.0f, "A non-finite radius falls back to the default");
  expect_eq(cfg.bloomThreshold, 0.80f, "A non-finite threshold falls back to the default");
  expect_eq(cfg.bloomStrength, 0.35f, "A non-finite strength falls back to the default");

  std::error_code error;
  std::filesystem::remove(tempPath, error);
}
```

Call it in `main()` after `test_config_detection_section();`:

```cpp
  test_config_world_post();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-host --target iee_tests`
Expected: compile error, `'struct iee::core::EngineConfig' has no member named 'softFogOfWar'`.

- [ ] **Step 3: Implement**

`src/iee/core/config.h`, after `bool interpolateAnimations = false;`:

```cpp
  bool softFogOfWar = false;
  float softFogRadius = 24.0f;  // world pixels; 0 = capture without blur
  bool bloom = false;
  float bloomThreshold = 0.80f;
  float bloomStrength = 0.35f;
```

`src/iee/core/config.cpp`, in `normalize`, after the `lodBias` clamp:

```cpp
  if (!std::isfinite(cfg.softFogRadius)) cfg.softFogRadius = 24.0f;
  if (!std::isfinite(cfg.bloomThreshold)) cfg.bloomThreshold = 0.80f;
  if (!std::isfinite(cfg.bloomStrength)) cfg.bloomStrength = 0.35f;
  cfg.softFogRadius = std::clamp(cfg.softFogRadius, 0.0f, 256.0f);
  cfg.bloomThreshold = std::clamp(cfg.bloomThreshold, 0.0f, 1.0f);
  cfg.bloomStrength = std::clamp(cfg.bloomStrength, 0.0f, 2.0f);
```

In `apply_kv`, in the `[Rendering]` chain, after the `InterpolateAnimations` branch and before `return;`:

```cpp
    else if (iequals(key, "SoftFogOfWar"))
      assign_bool(cfg.softFogOfWar);
    else if (iequals(key, "SoftFogRadius"))
      assign_float(cfg.softFogRadius);
    else if (iequals(key, "Bloom"))
      assign_bool(cfg.bloom);
    else if (iequals(key, "BloomThreshold"))
      assign_float(cfg.bloomThreshold);
    else if (iequals(key, "BloomStrength"))
      assign_float(cfg.bloomStrength);
```

In the save function, after `write_bool(f, "InterpolateAnimations", cfg.interpolateAnimations);`:

```cpp
  write_bool(f, "SoftFogOfWar", cfg.softFogOfWar);
  f << "SoftFogRadius = " << cfg.softFogRadius << "\n";
  write_bool(f, "Bloom", cfg.bloom);
  f << "BloomThreshold = " << cfg.bloomThreshold << "\n";
  f << "BloomStrength = " << cfg.bloomStrength << "\n";
```

Note: `parse_float` already rejects `nan`/`inf` text in some forms (see `test_config_numeric_bounds`, where `LODBias = nan` counts as handled by `normalize`). If the non-finite test cases fail because the parser rejects the text and keeps the default, that is the same observable result; keep the assertions as written.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build-host --target iee_tests && ctest --test-dir build-host --output-on-failure`
Expected: `All InfinityEngine-Enhancer native tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/iee/core/config.h src/iee/core/config.cpp tests/iee_tests.cpp
git commit -m "Config: soft fog of war and bloom settings"
```

---

### Task 4: Manifest entries and address resolution

Both patterns were verified offline on 2026-10-07 against the installed Steam 2.7.3 `Baldur.exe` (byte-identical to `ghidra-project/input/Baldur.exe`): each matches exactly once in `.text`, at `0x2A1B60` and `0x42B350`.

**Files:**
- Modify: `src/iee/game/build_manifest.h` (`PatternSet`, `ReferenceRvas`), `src/iee/game/build_manifest.cpp` (2.7.3 entry and its `static_assert`s), `src/iee/game/game_addrs.h`, `src/iee/game/game_addrs.cpp`
- Test: `tests/iee_tests.cpp`

**Interfaces:**
- Consumes: `EngineConfig::softFogOfWar`, `bloom`, `enableDebugHotkeys` (Task 3).
- Produces: `PatternSet::renderFog`, `PatternSet::drawFlush`; `ReferenceRvas::renderFog`, `ReferenceRvas::drawFlush`; `GameAddresses::RenderFog`, `GameAddresses::DrawFlush` (both 0 unless both resolved).

- [ ] **Step 1: Write the failing test**

Add before `int main()`:

```cpp
void test_manifest_world_post_targets() {
  const auto found = iee::game::find_manifest("BGEE 2.7.3.x");
  expect_true(found.has_value(), "The 2.7.3 manifest should be registered");
  if (!found) return;
  const auto& manifest = found->get();
  expect_true(!manifest.patterns.renderFog.empty(), "2.7.3 should carry a RenderFog pattern");
  expect_true(!manifest.patterns.drawFlush.empty(), "2.7.3 should carry a DrawFlush pattern");
  expect_eq(manifest.referenceRvas.renderFog, std::uintptr_t{0x2A1B60}, "RenderFog reference RVA");
  expect_eq(manifest.referenceRvas.drawFlush, std::uintptr_t{0x42B350}, "DrawFlush reference RVA");
  expect_true(iee::game::current_manifest().patterns.renderFog.empty(),
              "The 2.6.6 manifest has no world post targets");
}
```

Call it in `main()` after `test_manifest_infgame_offsets();`:

```cpp
  test_manifest_world_post_targets();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-host --target iee_tests`
Expected: compile error, `'struct iee::game::PatternSet' has no member named 'renderFog'`.

- [ ] **Step 3: Extend the manifest types**

`src/iee/game/build_manifest.h`, at the end of `PatternSet`:

```cpp
  // Optional: CInfinity::RenderFog and the GL backend's queue flush
  // (DrawFlush_GL). Hooked/called together for the world post passes (soft
  // fog of war, bloom); either empty or non-unique leaves both off.
  std::string_view renderFog{};
  std::string_view drawFlush{};
```

At the end of `ReferenceRvas`:

```cpp
  std::uintptr_t renderFog{};
  std::uintptr_t drawFlush{};
```

- [ ] **Step 4: Add the 2.7.3 values**

`src/iee/game/build_manifest.cpp`, in the `"BGEE 2.7.3.x"` entry. The patterns block is positional; append after the `vidCellFrameAccessor` string (the one ending `"48 8B F2 48 85 C9 75 15 48 89 1A 33 C0",`):

```cpp
            // CInfinity::RenderFog and DrawFlush_GL (PDB-named, each
            // offline-verified unique on the Steam 2.7.3.0 binary).
            "48 89 5C 24 10 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 "
            "48 8B D9 E8 ? ? ? ? B9 FF FF FF FF E8 ? ? ? ? B9 E1 0D 00 00",
            "4C 8B DC 55 41 56 41 57 49 8D 6B D8 48 81 EC 10 01 00 00 48 8B 05 ? ? ? ? "
            "48 33 C4 48 89 45 90 45 33 F6 44 39 35 ? ? ? ? 45 8B FE 0F 84",
```

Extend the reference RVA list of the same entry from

```cpp
        {0x27EBD0, 0x4257C0, 0x276700, 0x1F27D0, 0x36BA50, 0x36F170, 0x36E820, 0x4118A0, 0x411660,
         0x411780},
```

to

```cpp
        {0x27EBD0, 0x4257C0, 0x276700, 0x1F27D0, 0x36BA50, 0x36F170, 0x36E820, 0x4118A0, 0x411660,
         0x411780, 0x2A1B60, 0x42B350},
```

Add next to the other 2.7.3 `static_assert`s:

```cpp
static_assert(validate_pattern_format(kKnownBuilds[1].patterns.renderFog) &&
                  validate_pattern_format(kKnownBuilds[1].patterns.drawFlush),
              "2.7.3 world post pattern format is invalid");
```

- [ ] **Step 5: Resolve the addresses**

`src/iee/game/game_addrs.h`, in `GameAddresses` before `bool initialized`:

```cpp
        // Optional, all-or-nothing: the world post passes (soft fog of war,
        // bloom) need the fog render to hook and the queue flush to call.
        std::uintptr_t RenderFog = 0;
        std::uintptr_t DrawFlush = 0;
```

`src/iee/game/game_addrs.cpp`: the `resolveOptional` lambda is currently defined inside the `if (cfg.smoothSpriteMovement || cfg.interpolateAnimations)` block. Move its definition to just above that `if` (unchanged body), then add after the closing brace of that block and before `const bool success = ...`:

```cpp
        // Optional targets: world post passes. Resolved only when requested
        // (or when debug hotkeys can switch them on), and only as a pair.
        if (cfg.softFogOfWar || cfg.bloom || cfg.enableDebugHotkeys) {
            out.RenderFog = resolveOptional("CInfinity::RenderFog", manifest.patterns.renderFog,
                                            manifest.referenceRvas.renderFog);
            out.DrawFlush = resolveOptional("DrawFlush_GL", manifest.patterns.drawFlush,
                                            manifest.referenceRvas.drawFlush);
            if (out.RenderFog && out.DrawFlush) {
                LOG_INFO("World post targets resolved at RVA 0x{:X} / 0x{:X}",
                         out.RenderFog - moduleBase, out.DrawFlush - moduleBase);
            } else {
                const bool known =
                    !manifest.patterns.renderFog.empty() && !manifest.patterns.drawFlush.empty();
                out.RenderFog = out.DrawFlush = 0;
                if (cfg.softFogOfWar || cfg.bloom) {
                    LOG_WARN("Soft fog of war and bloom disabled: {}",
                             known ? "RenderFog/DrawFlush did not resolve uniquely"
                                   : "this build has no RenderFog/DrawFlush targets");
                }
            }
        }
```

- [ ] **Step 6: Run the tests and the Windows compile check**

Run: `cmake --build build-host --target iee_tests && ctest --test-dir build-host --output-on-failure`
Expected: `All InfinityEngine-Enhancer native tests passed`.

Run: `cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake -DIEE_BUILD_WINDOWS_DLL=ON -DIEE_WARNINGS_AS_ERRORS=ON && cmake --build build-mingw`
Expected: builds with no warnings.

- [ ] **Step 7: Commit**

```bash
git add src/iee/game/build_manifest.h src/iee/game/build_manifest.cpp src/iee/game/game_addrs.h src/iee/game/game_addrs.cpp tests/iee_tests.cpp
git commit -m "Manifest: CInfinity::RenderFog and DrawFlush_GL for 2.7.3"
```

---

### Task 5: GL entry points and the pass state guard

Windows-only code; there is no host test. The deliverable is a clean mingw build and a log line reporting availability.

**Files:**
- Modify: `src/iee/game/opengl_types.h`, `src/iee/game/opengl_types.cpp`
- Create: `src/iee/core/gl_pass_guard.h`

**Interfaces:**
- Produces in `iee::game::gl`: the constants and `OpenGLFunctions` members listed below, and `bool OpenGLFunctions::postProcessAvailable`.
- Produces `iee::core::GlPassGuard`: default-constructible RAII type. Construct only when `postProcessAvailable` is true. Saves and restores current program, vertex array binding, array buffer binding, viewport, blend enable and functions, scissor/depth-test/cull-face enables, clear colour. It does **not** manage framebuffer bindings or texture bindings (use the existing `core::GlStateGuard({0})` for unit 0).

- [ ] **Step 1: Add constants**

`src/iee/game/opengl_types.h`, after `constexpr unsigned FRAMEBUFFER = 0x8D40;`:

```cpp
constexpr unsigned READ_FRAMEBUFFER = 0x8CA8;
constexpr unsigned DRAW_FRAMEBUFFER = 0x8CA9;
constexpr unsigned DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr unsigned READ_FRAMEBUFFER_BINDING = 0x8CAA;
constexpr unsigned COLOR_ATTACHMENT0 = 0x8CE0;
constexpr unsigned FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr unsigned RGBA16F = 0x881A;
constexpr unsigned HALF_FLOAT = 0x140B;
constexpr unsigned FLOAT = 0x1406;
constexpr unsigned VIEWPORT = 0x0BA2;
constexpr unsigned SCISSOR_TEST = 0x0C11;
constexpr unsigned BLEND = 0x0BE2;
constexpr unsigned DEPTH_TEST = 0x0B71;
constexpr unsigned CULL_FACE = 0x0B44;
constexpr unsigned BLEND_DST_RGB = 0x80C8;
constexpr unsigned BLEND_SRC_RGB = 0x80C9;
constexpr unsigned BLEND_DST_ALPHA = 0x80CA;
constexpr unsigned BLEND_SRC_ALPHA = 0x80CB;
constexpr unsigned CURRENT_PROGRAM = 0x8B8D;
constexpr unsigned VERTEX_ARRAY_BINDING = 0x85B5;
constexpr unsigned ARRAY_BUFFER = 0x8892;
constexpr unsigned ARRAY_BUFFER_BINDING = 0x8894;
constexpr unsigned STATIC_DRAW = 0x88E4;
constexpr unsigned COLOR_CLEAR_VALUE = 0x0C22;
constexpr unsigned COLOR_BUFFER_BIT = 0x4000;
constexpr unsigned TRIANGLES = 0x0004;
constexpr unsigned ZERO = 0;
constexpr unsigned ONE = 1;
constexpr unsigned DST_COLOR = 0x0306;
constexpr unsigned FRAGMENT_SHADER = 0x8B30;
constexpr unsigned VERTEX_SHADER = 0x8B31;
constexpr unsigned COMPILE_STATUS = 0x8B81;
constexpr unsigned LINK_STATUS = 0x8B82;
```

- [ ] **Step 2: Add function pointer types and members**

In `opengl_types.h`, after the existing `using PFN_glBindFramebuffer = ...` line:

```cpp
using PFN_glViewport = void(APIENTRY*)(int x, int y, int width, int height);
using PFN_glEnable = void(APIENTRY*)(unsigned cap);
using PFN_glDisable = void(APIENTRY*)(unsigned cap);
using PFN_glIsEnabled = unsigned char(APIENTRY*)(unsigned cap);
using PFN_glBlendFunc = void(APIENTRY*)(unsigned sfactor, unsigned dfactor);
using PFN_glClearColor = void(APIENTRY*)(float r, float g, float b, float a);
using PFN_glClear = void(APIENTRY*)(unsigned mask);
using PFN_glDrawArrays = void(APIENTRY*)(unsigned mode, int first, int count);
using PFN_glGetFloatv = void(APIENTRY*)(unsigned pname, float* data);
using PFN_glGenFramebuffers = void(APIENTRY*)(int n, unsigned* framebuffers);
using PFN_glDeleteFramebuffers = void(APIENTRY*)(int n, const unsigned* framebuffers);
using PFN_glFramebufferTexture2D = void(APIENTRY*)(unsigned target, unsigned attachment,
                                                   unsigned textarget, unsigned texture, int level);
using PFN_glCheckFramebufferStatus = unsigned(APIENTRY*)(unsigned target);
using PFN_glBlitFramebuffer = void(APIENTRY*)(int srcX0, int srcY0, int srcX1, int srcY1,
                                              int dstX0, int dstY0, int dstX1, int dstY1,
                                              unsigned mask, unsigned filter);
using PFN_glGenVertexArrays = void(APIENTRY*)(int n, unsigned* arrays);
using PFN_glDeleteVertexArrays = void(APIENTRY*)(int n, const unsigned* arrays);
using PFN_glBindVertexArray = void(APIENTRY*)(unsigned array);
using PFN_glGenBuffers = void(APIENTRY*)(int n, unsigned* buffers);
using PFN_glDeleteBuffers = void(APIENTRY*)(int n, const unsigned* buffers);
using PFN_glBindBuffer = void(APIENTRY*)(unsigned target, unsigned buffer);
using PFN_glBufferData = void(APIENTRY*)(unsigned target, std::ptrdiff_t size, const void* data,
                                         unsigned usage);
using PFN_glEnableVertexAttribArray = void(APIENTRY*)(unsigned index);
using PFN_glVertexAttribPointer = void(APIENTRY*)(unsigned index, int size, unsigned type,
                                                  unsigned char normalized, int stride,
                                                  const void* pointer);
using PFN_glBlendFuncSeparate = void(APIENTRY*)(unsigned srcRgb, unsigned dstRgb,
                                                unsigned srcAlpha, unsigned dstAlpha);
```

Add `#include <cstddef>` at the top of the header.

In `struct OpenGLFunctions`, after `PFN_glPixelStorei glPixelStorei{};`:

```cpp
  PFN_glViewport glViewport{};
  PFN_glEnable glEnable{};
  PFN_glDisable glDisable{};
  PFN_glIsEnabled glIsEnabled{};
  PFN_glBlendFunc glBlendFunc{};
  PFN_glClearColor glClearColor{};
  PFN_glClear glClear{};
  PFN_glDrawArrays glDrawArrays{};
  PFN_glGetFloatv glGetFloatv{};
```

After `PFN_glBindFramebuffer glBindFramebuffer{};`:

```cpp
  PFN_glGenFramebuffers glGenFramebuffers{};
  PFN_glDeleteFramebuffers glDeleteFramebuffers{};
  PFN_glFramebufferTexture2D glFramebufferTexture2D{};
  PFN_glCheckFramebufferStatus glCheckFramebufferStatus{};
  PFN_glBlitFramebuffer glBlitFramebuffer{};
  PFN_glGenVertexArrays glGenVertexArrays{};
  PFN_glDeleteVertexArrays glDeleteVertexArrays{};
  PFN_glBindVertexArray glBindVertexArray{};
  PFN_glGenBuffers glGenBuffers{};
  PFN_glDeleteBuffers glDeleteBuffers{};
  PFN_glBindBuffer glBindBuffer{};
  PFN_glBufferData glBufferData{};
  PFN_glEnableVertexAttribArray glEnableVertexAttribArray{};
  PFN_glVertexAttribPointer glVertexAttribPointer{};
  PFN_glBlendFuncSeparate glBlendFuncSeparate{};
```

After `bool compressedTextureUploadAvailable{false};`:

```cpp
  // Everything the world post passes need: framebuffer objects, a vertex
  // array, shaders with introspection, and the fixed-function state calls.
  bool postProcessAvailable{false};
```

- [ ] **Step 3: Load them**

`src/iee/game/opengl_types.cpp`, in `OpenGLFunctions::initialize`, after the `glPixelStorei` load:

```cpp
  glViewport = reinterpret_cast<PFN_glViewport>(get_gl1_proc_address(opengl32, "glViewport"));
  glEnable = reinterpret_cast<PFN_glEnable>(get_gl1_proc_address(opengl32, "glEnable"));
  glDisable = reinterpret_cast<PFN_glDisable>(get_gl1_proc_address(opengl32, "glDisable"));
  glIsEnabled = reinterpret_cast<PFN_glIsEnabled>(get_gl1_proc_address(opengl32, "glIsEnabled"));
  glBlendFunc = reinterpret_cast<PFN_glBlendFunc>(get_gl1_proc_address(opengl32, "glBlendFunc"));
  glClearColor =
      reinterpret_cast<PFN_glClearColor>(get_gl1_proc_address(opengl32, "glClearColor"));
  glClear = reinterpret_cast<PFN_glClear>(get_gl1_proc_address(opengl32, "glClear"));
  glDrawArrays =
      reinterpret_cast<PFN_glDrawArrays>(get_gl1_proc_address(opengl32, "glDrawArrays"));
  glGetFloatv = reinterpret_cast<PFN_glGetFloatv>(get_gl1_proc_address(opengl32, "glGetFloatv"));
```

After the `glBindFramebuffer` load:

```cpp
  glGenFramebuffers = reinterpret_cast<PFN_glGenFramebuffers>(
      get_ext_proc_address(opengl32, "glGenFramebuffers"));
  glDeleteFramebuffers = reinterpret_cast<PFN_glDeleteFramebuffers>(
      get_ext_proc_address(opengl32, "glDeleteFramebuffers"));
  glFramebufferTexture2D = reinterpret_cast<PFN_glFramebufferTexture2D>(
      get_ext_proc_address(opengl32, "glFramebufferTexture2D"));
  glCheckFramebufferStatus = reinterpret_cast<PFN_glCheckFramebufferStatus>(
      get_ext_proc_address(opengl32, "glCheckFramebufferStatus"));
  glBlitFramebuffer = reinterpret_cast<PFN_glBlitFramebuffer>(
      get_ext_proc_address(opengl32, "glBlitFramebuffer"));
  glGenVertexArrays = reinterpret_cast<PFN_glGenVertexArrays>(
      get_ext_proc_address(opengl32, "glGenVertexArrays"));
  glDeleteVertexArrays = reinterpret_cast<PFN_glDeleteVertexArrays>(
      get_ext_proc_address(opengl32, "glDeleteVertexArrays"));
  glBindVertexArray = reinterpret_cast<PFN_glBindVertexArray>(
      get_ext_proc_address(opengl32, "glBindVertexArray"));
  glGenBuffers = reinterpret_cast<PFN_glGenBuffers>(get_ext_proc_address(opengl32, "glGenBuffers"));
  glDeleteBuffers =
      reinterpret_cast<PFN_glDeleteBuffers>(get_ext_proc_address(opengl32, "glDeleteBuffers"));
  glBindBuffer = reinterpret_cast<PFN_glBindBuffer>(get_ext_proc_address(opengl32, "glBindBuffer"));
  glBufferData = reinterpret_cast<PFN_glBufferData>(get_ext_proc_address(opengl32, "glBufferData"));
  glEnableVertexAttribArray = reinterpret_cast<PFN_glEnableVertexAttribArray>(
      get_ext_proc_address(opengl32, "glEnableVertexAttribArray"));
  glVertexAttribPointer = reinterpret_cast<PFN_glVertexAttribPointer>(
      get_ext_proc_address(opengl32, "glVertexAttribPointer"));
  glBlendFuncSeparate = reinterpret_cast<PFN_glBlendFuncSeparate>(
      get_ext_proc_address(opengl32, "glBlendFuncSeparate"));
```

After the `compressedTextureUploadAvailable = ...` line:

```cpp
  postProcessAvailable =
      textureUploadAvailable && readyForSourcePatching && glViewport && glEnable && glDisable &&
      glIsEnabled && glBlendFunc && glClearColor && glClear && glDrawArrays && glGetFloatv &&
      glBindFramebuffer && glGenFramebuffers && glDeleteFramebuffers && glFramebufferTexture2D &&
      glCheckFramebufferStatus && glBlitFramebuffer && glGenVertexArrays &&
      glDeleteVertexArrays && glBindVertexArray && glGenBuffers && glDeleteBuffers &&
      glBindBuffer && glBufferData && glEnableVertexAttribArray && glVertexAttribPointer &&
      glBlendFuncSeparate;
```

Extend the existing `LOG_INFO("OpenGL initialized (...` call: add `, post process={}` to the format string and `postProcessAvailable ? "ready" : "partial"` as the last argument.

- [ ] **Step 4: Write the guard**

`src/iee/core/gl_pass_guard.h`:

```cpp
#pragma once

#include "iee/game/opengl_types.h"

namespace iee::core {
// Saves the GL state our own draw passes change and puts it back on scope
// exit. The engine issues state changes relative to what it believes the
// hardware state is, so anything left altered corrupts its next draws.
// Framebuffer and texture bindings are not covered: the caller owns the
// framebuffer hand-over, and GlStateGuard covers texture units.
// Requires OpenGLFunctions::postProcessAvailable.
class GlPassGuard {
 public:
  GlPassGuard() noexcept {
    const auto& gl = game::gl::get_gl_functions();
    gl.glGetIntegerv(game::gl::CURRENT_PROGRAM, &program_);
    gl.glGetIntegerv(game::gl::VERTEX_ARRAY_BINDING, &vertexArray_);
    gl.glGetIntegerv(game::gl::ARRAY_BUFFER_BINDING, &arrayBuffer_);
    gl.glGetIntegerv(game::gl::VIEWPORT, viewport_);
    gl.glGetIntegerv(game::gl::BLEND_SRC_RGB, &blendSrcRgb_);
    gl.glGetIntegerv(game::gl::BLEND_DST_RGB, &blendDstRgb_);
    gl.glGetIntegerv(game::gl::BLEND_SRC_ALPHA, &blendSrcAlpha_);
    gl.glGetIntegerv(game::gl::BLEND_DST_ALPHA, &blendDstAlpha_);
    gl.glGetFloatv(game::gl::COLOR_CLEAR_VALUE, clearColor_);
    blend_ = gl.glIsEnabled(game::gl::BLEND) != 0;
    scissor_ = gl.glIsEnabled(game::gl::SCISSOR_TEST) != 0;
    depthTest_ = gl.glIsEnabled(game::gl::DEPTH_TEST) != 0;
    cullFace_ = gl.glIsEnabled(game::gl::CULL_FACE) != 0;
  }

  ~GlPassGuard() noexcept {
    const auto& gl = game::gl::get_gl_functions();
    const auto set = [&](unsigned capability, bool enabled) {
      if (enabled) {
        gl.glEnable(capability);
      } else {
        gl.glDisable(capability);
      }
    };
    set(game::gl::BLEND, blend_);
    set(game::gl::SCISSOR_TEST, scissor_);
    set(game::gl::DEPTH_TEST, depthTest_);
    set(game::gl::CULL_FACE, cullFace_);
    gl.glBlendFuncSeparate(
        static_cast<unsigned>(blendSrcRgb_), static_cast<unsigned>(blendDstRgb_),
        static_cast<unsigned>(blendSrcAlpha_), static_cast<unsigned>(blendDstAlpha_));
    gl.glClearColor(clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]);
    gl.glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
    gl.glBindVertexArray(static_cast<unsigned>(vertexArray_));
    gl.glBindBuffer(game::gl::ARRAY_BUFFER, static_cast<unsigned>(arrayBuffer_));
    gl.glUseProgram(static_cast<unsigned>(program_));
  }

  GlPassGuard(const GlPassGuard&) = delete;
  GlPassGuard& operator=(const GlPassGuard&) = delete;

  // The viewport as the engine left it: {x, y, width, height}.
  [[nodiscard]] const int* viewport() const noexcept { return viewport_; }

 private:
  int program_{};
  int vertexArray_{};
  int arrayBuffer_{};
  int viewport_[4]{};
  int blendSrcRgb_{};
  int blendDstRgb_{};
  int blendSrcAlpha_{};
  int blendDstAlpha_{};
  float clearColor_[4]{};
  bool blend_{};
  bool scissor_{};
  bool depthTest_{};
  bool cullFace_{};
};
}  // namespace iee::core
```

- [ ] **Step 5: Compile check**

The guard header is not included anywhere yet; add a temporary `#include "iee/core/gl_pass_guard.h"` at the top of `src/iee/game/opengl_types.cpp`, build, then remove it.

Run: `cmake --build build-mingw`
Expected: builds with no warnings. Remove the temporary include and rebuild.

- [ ] **Step 6: Commit**

```bash
git add src/iee/game/opengl_types.h src/iee/game/opengl_types.cpp src/iee/core/gl_pass_guard.h
git commit -m "GL: framebuffer and vertex-array entry points, pass state guard"
```

---

### Task 6: Fog capture without blur, wired to the hook (first in-game bundle)

This is spec §8 step 1: the engine's fog goes through our target and comes back unblurred. The picture must be indistinguishable from vanilla. Blur and bloom arrive in Tasks 7 and 8 and reuse everything built here.

**Files:**
- Create: `src/iee/features/world_post.h`, `src/iee/features/world_post.cpp`
- Modify: `src/iee/hooks.cpp`, `CMakeLists.txt` (DLL source list, after `src/iee/features/tile_render.cpp`)

**Interfaces:**
- Consumes: `game::Extent`, `game::pixels_per_world_pixel` (Task 2); `EngineConfig` fields (Task 3); `GameAddresses::RenderFog`, `DrawFlush` (Task 4); `gl::OpenGLFunctions::postProcessAvailable`, `core::GlPassGuard` (Task 5); existing `core::GlStateGuard`, `area::read_view_transform`, `area::ViewTransform`.
- Produces (namespace `iee::features`, all `noexcept`, render thread only unless noted):
  - `void world_post_configure(const core::EngineConfig& cfg) noexcept;`
  - `bool world_post_active() noexcept;` — true when an effect is on and nothing has failed
  - `bool world_post_before_fog(float viewWorldWidth) noexcept;` — call right after flushing; returns true when the fog target is now bound and the caller must flush after the original `RenderFog` and then call `world_post_after_fog()`
  - `void world_post_after_fog() noexcept;`
  - `void world_post_on_area_load() noexcept;` — any thread; re-arms the once-per-area log
  - `void world_post_forget() noexcept;` — drops handles without GL calls (shutdown)

- [ ] **Step 1: Write the header**

`src/iee/features/world_post.h`:

```cpp
#pragma once

namespace iee::core {
struct EngineConfig;
}

namespace iee::features {
// World post passes: soft fog of war and bloom, run from the
// CInfinity::RenderFog detour. The engine queues the whole frame and only
// draws in DrawFlush_GL, so the caller flushes that queue around these calls:
//
//   flush();                                  // world is now in the framebuffer
//   bool capturing = world_post_before_fog(); // bloom; fog target bound if true
//   original RenderFog                        // queues the fog triangles
//   if (capturing) { flush(); world_post_after_fog(); }
//
// Render thread only, except world_post_on_area_load.

void world_post_configure(const core::EngineConfig& cfg) noexcept;
// True when at least one effect is switched on and no pass has failed.
bool world_post_active() noexcept;
// viewWorldWidth: world pixels visible across the view (0 when unknown).
bool world_post_before_fog(float viewWorldWidth) noexcept;
void world_post_after_fog() noexcept;
// Any thread. The next frame logs what engaged.
void world_post_on_area_load() noexcept;
// Drops every GL handle without touching GL. For shutdown paths.
void world_post_forget() noexcept;
}  // namespace iee::features
```

- [ ] **Step 2: Write the implementation**

`src/iee/features/world_post.cpp`:

```cpp
#include "iee/features/world_post.h"

#include <windows.h>

#include <atomic>

#include "iee/core/config.h"
#include "iee/core/gl_pass_guard.h"
#include "iee/core/gl_state_guard.h"
#include "iee/core/logger.h"
#include "iee/game/opengl_types.h"
#include "iee/game/world_post_plan.h"

namespace iee::features {
namespace {
namespace gl = game::gl;

struct Target {
  unsigned texture{};
  unsigned framebuffer{};
  game::Extent extent{};
};

struct CompositeProgram {
  unsigned id{};
  int scale{-1};
  int dither{-1};
};

struct State {
  bool softFog{};
  bool bloom{};
  float fogRadius{};
  float bloomThreshold{};
  float bloomStrength{};

  bool failed{};
  HGLRC context{};
  game::Extent viewport{};
  int engineDrawFramebuffer{};
  int engineReadFramebuffer{};
  float pixelsPerWorldPixel{1.0f};

  unsigned vertexArray{};
  unsigned vertexBuffer{};
  CompositeProgram composite{};
  Target fogCapture{};
  bool fogDrawnOnce{};
};

State g_state;
std::atomic<bool> g_logNextFrame{true};

constexpr const char* kVertexSource = R"glsl(#version 330
layout(location = 0) in vec2 aPosition;
out vec2 vUv;
void main() {
  vUv = aPosition * 0.5 + 0.5;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)glsl";

// Writes the texture scaled by uScale; the caller's blend mode decides whether
// that multiplies (fog) or adds (bloom). uDither adds +-0.5/255 of ordered
// noise to hide banding in long dark gradients.
constexpr const char* kCompositeSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform float uScale;
uniform float uDither;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec3 color = texture(uTexture, vUv).rgb * uScale;
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  fragColor = vec4(color + uDither * (noise - 0.5) / 255.0, 1.0);
}
)glsl";

void fail(const char* reason) noexcept {
  g_state.failed = true;
  try {
    LOG_WARN("Soft fog of war and bloom disabled for this session: {}", reason);
  } catch (...) {
  }
}

unsigned compile_shader(unsigned type, const char* source) noexcept {
  const auto& fn = gl::get_gl_functions();
  const unsigned shader = fn.glCreateShader(type);
  if (!shader) return 0;
  fn.glShaderSource(shader, 1, &source, nullptr);
  fn.glCompileShader(shader);
  int status = 0;
  fn.glGetShaderiv(shader, gl::COMPILE_STATUS, &status);
  if (!status) {
    char log[512]{};
    fn.glGetShaderInfoLog(shader, static_cast<int>(sizeof(log)) - 1, nullptr, log);
    try {
      LOG_WARN("World post shader compile failed: {}", log);
    } catch (...) {
    }
    fn.glDeleteShader(shader);
    return 0;
  }
  return shader;
}

unsigned build_program(const char* fragmentSource) noexcept {
  const auto& fn = gl::get_gl_functions();
  const unsigned vertex = compile_shader(gl::VERTEX_SHADER, kVertexSource);
  const unsigned fragment = compile_shader(gl::FRAGMENT_SHADER, fragmentSource);
  unsigned program = 0;
  if (vertex && fragment) {
    program = fn.glCreateProgram();
    fn.glAttachShader(program, vertex);
    fn.glAttachShader(program, fragment);
    fn.glLinkProgram(program);
    int status = 0;
    fn.glGetProgramiv(program, gl::LINK_STATUS, &status);
    if (!status) {
      char log[512]{};
      fn.glGetProgramInfoLog(program, static_cast<int>(sizeof(log)) - 1, nullptr, log);
      try {
        LOG_WARN("World post program link failed: {}", log);
      } catch (...) {
      }
      fn.glDeleteProgram(program);
      program = 0;
    }
  }
  if (vertex) fn.glDeleteShader(vertex);
  if (fragment) fn.glDeleteShader(fragment);
  return program;
}

void destroy_target(Target& target) noexcept {
  const auto& fn = gl::get_gl_functions();
  if (target.framebuffer) fn.glDeleteFramebuffers(1, &target.framebuffer);
  if (target.texture) fn.glDeleteTextures(1, &target.texture);
  target = {};
}

// Leaves the new framebuffer bound; the caller rebinds what it needs.
bool create_target(Target& target, game::Extent extent, unsigned internalFormat,
                   unsigned type) noexcept {
  const auto& fn = gl::get_gl_functions();
  destroy_target(target);
  fn.glGenTextures(1, &target.texture);
  fn.glBindTexture(gl::TEXTURE_2D, target.texture);
  fn.glTexImage2D(gl::TEXTURE_2D, 0, static_cast<int>(internalFormat), extent.width,
                  extent.height, 0, gl::RGBA, type, nullptr);
  fn.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<int>(gl::LINEAR));
  fn.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<int>(gl::LINEAR));
  fn.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, static_cast<int>(gl::CLAMP_TO_EDGE));
  fn.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, static_cast<int>(gl::CLAMP_TO_EDGE));
  fn.glGenFramebuffers(1, &target.framebuffer);
  fn.glBindFramebuffer(gl::FRAMEBUFFER, target.framebuffer);
  fn.glFramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D,
                            target.texture, 0);
  if (fn.glCheckFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE) {
    destroy_target(target);
    return false;
  }
  target.extent = extent;
  return true;
}

void bind_engine_framebuffer() noexcept {
  const auto& fn = gl::get_gl_functions();
  fn.glBindFramebuffer(gl::DRAW_FRAMEBUFFER,
                       static_cast<unsigned>(g_state.engineDrawFramebuffer));
  fn.glBindFramebuffer(gl::READ_FRAMEBUFFER,
                       static_cast<unsigned>(g_state.engineReadFramebuffer));
}

// State every pass of ours draws with. The guard in scope restores it.
void set_pass_state() noexcept {
  const auto& fn = gl::get_gl_functions();
  fn.glDisable(gl::SCISSOR_TEST);
  fn.glDisable(gl::DEPTH_TEST);
  fn.glDisable(gl::CULL_FACE);
  fn.glDisable(gl::BLEND);
  fn.glBindVertexArray(g_state.vertexArray);
  fn.glActiveTexture(gl::TEXTURE0);
}

void draw_composite(unsigned texture, float scale, float dither) noexcept {
  const auto& fn = gl::get_gl_functions();
  fn.glUseProgram(g_state.composite.id);
  fn.glUniform1f(g_state.composite.scale, scale);
  fn.glUniform1f(g_state.composite.dither, dither);
  fn.glBindTexture(gl::TEXTURE_2D, texture);
  fn.glDrawArrays(gl::TRIANGLES, 0, 3);
}

bool create_shared_resources() noexcept {
  const auto& fn = gl::get_gl_functions();
  static constexpr float kTriangle[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
  fn.glGenVertexArrays(1, &g_state.vertexArray);
  fn.glGenBuffers(1, &g_state.vertexBuffer);
  fn.glBindVertexArray(g_state.vertexArray);
  fn.glBindBuffer(gl::ARRAY_BUFFER, g_state.vertexBuffer);
  fn.glBufferData(gl::ARRAY_BUFFER, sizeof(kTriangle), kTriangle, gl::STATIC_DRAW);
  fn.glEnableVertexAttribArray(0);
  fn.glVertexAttribPointer(0, 2, gl::FLOAT, 0, 0, nullptr);

  g_state.composite.id = build_program(kCompositeSource);
  if (!g_state.composite.id) return false;
  g_state.composite.scale = fn.glGetUniformLocation(g_state.composite.id, "uScale");
  g_state.composite.dither = fn.glGetUniformLocation(g_state.composite.id, "uDither");
  fn.glUseProgram(g_state.composite.id);
  fn.glUniform1i(fn.glGetUniformLocation(g_state.composite.id, "uTexture"), 0);
  return true;
}

bool create_sized_resources(game::Extent viewport) noexcept {
  if (!create_target(g_state.fogCapture, viewport, gl::RGBA8, gl::UNSIGNED_BYTE)) return false;
  g_state.viewport = viewport;
  return true;
}

// Called inside a GlPassGuard + GlStateGuard scope: creation changes bindings.
bool ensure_resources(game::Extent viewport) noexcept {
  const auto context = gl::current_context();
  if (context != g_state.context) {
    // The old context is gone and took its objects with it.
    const State kept = g_state;
    g_state = {};
    g_state.softFog = kept.softFog;
    g_state.bloom = kept.bloom;
    g_state.fogRadius = kept.fogRadius;
    g_state.bloomThreshold = kept.bloomThreshold;
    g_state.bloomStrength = kept.bloomStrength;
    g_state.engineDrawFramebuffer = kept.engineDrawFramebuffer;
    g_state.engineReadFramebuffer = kept.engineReadFramebuffer;
    g_state.context = context;
    gl::discard_errors();
    if (!create_shared_resources() || !gl::check_error("world post shared resources")) {
      return false;
    }
  }
  if (!(viewport == g_state.viewport)) {
    gl::discard_errors();
    if (!create_sized_resources(viewport) || !gl::check_error("world post targets")) return false;
    g_logNextFrame.store(true, std::memory_order_relaxed);
  }
  return true;
}
}  // namespace

void world_post_configure(const core::EngineConfig& cfg) noexcept {
  g_state.softFog = cfg.softFogOfWar;
  g_state.bloom = cfg.bloom;
  g_state.fogRadius = cfg.softFogRadius;
  g_state.bloomThreshold = cfg.bloomThreshold;
  g_state.bloomStrength = cfg.bloomStrength;
}

bool world_post_active() noexcept {
  return !g_state.failed && (g_state.softFog || g_state.bloom);
}

bool world_post_before_fog(float viewWorldWidth) noexcept {
  const auto& fn = gl::get_gl_functions();
  if (!fn.postProcessAvailable) {
    fail("the GL context lacks framebuffer or vertex-array support");
    return false;
  }
  fn.glGetIntegerv(gl::DRAW_FRAMEBUFFER_BINDING, &g_state.engineDrawFramebuffer);
  fn.glGetIntegerv(gl::READ_FRAMEBUFFER_BINDING, &g_state.engineReadFramebuffer);

  bool capturing = false;
  {
    core::GlStateGuard textures({0});
    core::GlPassGuard pass;
    const int* viewport = pass.viewport();
    if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] <= 0 || viewport[3] <= 0) {
      fail("the engine viewport is not a full-window rectangle at the origin");
    } else if (!ensure_resources({viewport[2], viewport[3]})) {
      fail("GL resources could not be created");
    } else {
      g_state.pixelsPerWorldPixel =
          game::pixels_per_world_pixel(static_cast<float>(viewport[2]), viewWorldWidth);
      set_pass_state();
      if (g_state.softFog) {
        // White = "nothing hidden". The engine's alpha-blended black fog then
        // leaves, per pixel, the fraction of the scene that stays visible.
        fn.glBindFramebuffer(gl::FRAMEBUFFER, g_state.fogCapture.framebuffer);
        fn.glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        fn.glClear(gl::COLOR_BUFFER_BIT);
        capturing = true;
      }
      if (g_logNextFrame.exchange(false, std::memory_order_relaxed)) {
        try {
          LOG_INFO(
              "World post: softFog={} (radius {} world px), bloom={}, viewport {}x{}, "
              "{:.3f} px per world px, engine framebuffer draw={} read={}",
              g_state.softFog, g_state.fogRadius, g_state.bloom, viewport[2], viewport[3],
              g_state.pixelsPerWorldPixel, g_state.engineDrawFramebuffer,
              g_state.engineReadFramebuffer);
        } catch (...) {
        }
      }
    }
    bind_engine_framebuffer();
  }
  // The guards have restored the engine's state; hand it our target for the
  // fog draw only. Framebuffer bindings are not part of the engine's cache.
  if (capturing) fn.glBindFramebuffer(gl::FRAMEBUFFER, g_state.fogCapture.framebuffer);
  return capturing;
}

void world_post_after_fog() noexcept {
  const auto& fn = gl::get_gl_functions();
  core::GlStateGuard textures({0});
  core::GlPassGuard pass;
  bind_engine_framebuffer();
  set_pass_state();
  fn.glViewport(0, 0, g_state.viewport.width, g_state.viewport.height);
  // framebuffer = framebuffer * visible fraction
  fn.glEnable(gl::BLEND);
  fn.glBlendFunc(gl::DST_COLOR, gl::ZERO);
  draw_composite(g_state.fogCapture.texture, 1.0f, 0.0f);
  if (!g_state.fogDrawnOnce) {
    g_state.fogDrawnOnce = true;
    if (!gl::check_error("world post fog composite")) {
      fail("the fog composite raised a GL error");
    } else {
      try {
        LOG_INFO("World post: first fog composite drawn");
      } catch (...) {
      }
    }
  }
}

void world_post_on_area_load() noexcept { g_logNextFrame.store(true, std::memory_order_relaxed); }

void world_post_forget() noexcept {
  const State kept = g_state;
  g_state = {};
  g_state.softFog = kept.softFog;
  g_state.bloom = kept.bloom;
  g_state.fogRadius = kept.fogRadius;
  g_state.bloomThreshold = kept.bloomThreshold;
  g_state.bloomStrength = kept.bloomStrength;
}
}  // namespace iee::features
```

Notes for the implementer:
- `fogDrawnOnce` is reset when the context changes (the whole `State` is rebuilt), which is intended.
- `fail()` inside `world_post_before_fog` still falls through to `bind_engine_framebuffer()`, so a failure never leaves our target bound.
- If `fail()` fires inside `world_post_after_fog`, the frame has already been composited; later frames skip the effect.
- If `gl::RGBA` / `gl::RGBA8` / `gl::UNSIGNED_BYTE` constants are declared with different spellings in `opengl_types.h`, use the existing ones.

Add to `CMakeLists.txt` in the DLL source list after `src/iee/features/tile_render.cpp`:

```cmake
        src/iee/features/world_post.cpp
```

- [ ] **Step 3: Wire the hook**

`src/iee/hooks.cpp`:

Add the include next to the other feature include:

```cpp
#include "iee/features/world_post.h"
```

After the `g_vidCellFrameSizeHook` declaration:

```cpp
// CInfinity::RenderFog(CVidMode*, CVisibilityMap*) and the GL backend's
// argument-less queue flush, used together by the world post passes.
using RenderFogFn = void (*)(void*, void*, void*);
using DrawFlushFn = void (*)();
static core::Hook<RenderFogFn> g_renderFogHook;
static DrawFlushFn g_drawFlush = nullptr;
```

Before `detour_static_render`:

```cpp
// World post passes (soft fog of war, bloom). The engine queues every draw
// and submits the queue in DrawFlush_GL at the end of the frame, so the world
// only exists in the framebuffer once we flush, and the fog only lands in our
// target if we flush again while it is bound. With both effects off this
// detour is a plain pass-through: no flush, the frame is the engine's own.
static void detour_render_fog(void* infinity, void* vidMode, void* visibility) {
  const auto original = g_renderFogHook.original();
  if (!g_ctx || !g_drawFlush || !features::world_post_active()) {
    original(infinity, vidMode, visibility);
    return;
  }
  float viewWorldWidth = 0.0f;
  if (const auto* activeArea = g_ctx->activeArea.load()) {
    area::ViewTransform view{};
    if (area::read_view_transform(activeArea, view)) viewWorldWidth = view.viewWorldW;
  }
  g_drawFlush();
  const bool capturing = features::world_post_before_fog(viewWorldWidth);
  original(infinity, vidMode, visibility);
  if (capturing) {
    g_drawFlush();
    features::world_post_after_fog();
  }
}
```

In `install_all`, after the `if (ctx.cfg.smoothSpriteMovement) { ... }` block and before `g_loadAreaHook.enable();`:

```cpp
    if (ctx.addrs.RenderFog && ctx.addrs.DrawFlush) {
      try {
        features::world_post_configure(ctx.cfg);
        g_drawFlush = reinterpret_cast<DrawFlushFn>(ctx.addrs.DrawFlush);
        g_renderFogHook.create(reinterpret_cast<void*>(ctx.addrs.RenderFog),
                               reinterpret_cast<void*>(&detour_render_fog));
        g_renderFogHook.enable();
        LOG_INFO("CInfinity::RenderFog hook installed (soft fog of war={}, bloom={})",
                 ctx.cfg.softFogOfWar, ctx.cfg.bloom);
      } catch (...) {
        g_drawFlush = nullptr;
        (void)g_renderFogHook.remove();
        LOG_WARN("CInfinity::RenderFog hook failed; fog of war and bloom stay vanilla");
      }
    }
```

In `uninstall_all`, before `g_animationInterpActive = false;`:

```cpp
  (void)g_renderFogHook.remove();
  g_drawFlush = nullptr;
  features::world_post_forget();
```

In `prepare_for_shutdown`, as the first statement:

```cpp
  (void)g_renderFogHook.disable();
```

In `detour_load_area`, next to where `g_spriteMotionReset` is raised:

```cpp
  features::world_post_on_area_load();
```

- [ ] **Step 4: Build**

Run: `cmake --build build-host --target iee_tests && ctest --test-dir build-host --output-on-failure`
Expected: `All InfinityEngine-Enhancer native tests passed`.

Run: `cmake --build build-mingw`
Expected: builds with no warnings.

- [ ] **Step 5: Commit and produce the bundle**

```bash
git add src/iee/features/world_post.h src/iee/features/world_post.cpp src/iee/hooks.cpp CMakeLists.txt
git commit -m "World post: capture the engine fog through our own target"
git push -u origin feature/world-post
```

Open a draft PR against `feature/are-animation-detection` so CI builds the bundle. (Pushes go through the 1Password SSH agent; an unanswered approval prompt shows up as `Permission denied (publickey)`.)

- [ ] **Step 6: Owner in-game check (blocking)**

INI: `[Rendering] SoftFogOfWar = true`, `SoftFogRadius = 0`, `Bloom = false`. Compare against `SoftFogOfWar = false` in the same spot.

Checklist:
1. Log shows `CInfinity::RenderFog hook installed`, one `World post: softFog=true …` line per area, and `first fog composite drawn`. No `disabled for this session` line.
2. Fog of war looks the same as vanilla: same shape, same darkness for unexplored and explored-but-unseen.
3. Characters behind walls are still hidden/dithered correctly; nothing draws over the UI or vanishes behind the world.
4. Rain or snow still draws (AR2600 area in bad weather, or any outdoor area).
5. A fully explored interior shows no darkening at all.
6. A screenshot (the game's own key) looks like the screen.
7. Resize the window or change resolution: fog still lines up.
8. Save and load, change area: no stale fog, log re-arms.

If item 3 fails, stop: the offline depth finding in Task 1 is contradicted, so report the symptom before going further. If any other item fails, fix before Task 7.

---

### Task 7: Soft fog (blur chain)

**Files:**
- Modify: `src/iee/features/world_post.cpp`

**Interfaces:**
- Consumes: `game::BlurPlan`, `blur_plan_for_radius`, `half_extent`, `available_blur_levels`, `kMaxBlurLevels` (Task 2); everything from Task 6.
- Produces (file-local, reused by Task 8):
  - `struct BlurChain { std::array<Target, game::kMaxBlurLevels> levels{}; int count{}; };`
  - `bool create_chain(BlurChain& chain, game::Extent base) noexcept;` — level `i` is `base` halved `i + 1` times, `RGBA16F`
  - `void destroy_chain(BlurChain& chain) noexcept;`
  - `void run_blur(const Target& source, BlurChain& chain, game::BlurPlan plan, bool accumulate) noexcept;` — result in `chain.levels[0]`; leaves an arbitrary framebuffer bound; requires pass state set

- [ ] **Step 1: Add the blur programs and chain**

In `world_post.cpp`, add `#include <array>` and, after `kCompositeSource`:

```cpp
// Dual-filter blur (down then up through half-size levels). uStep is the
// sample spread in source UV units.
constexpr const char* kBlurDownSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform vec2 uStep;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec4 sum = texture(uTexture, vUv) * 4.0;
  sum += texture(uTexture, vUv - uStep);
  sum += texture(uTexture, vUv + uStep);
  sum += texture(uTexture, vUv + vec2(uStep.x, -uStep.y));
  sum += texture(uTexture, vUv - vec2(uStep.x, -uStep.y));
  fragColor = sum / 8.0;
}
)glsl";

constexpr const char* kBlurUpSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform vec2 uStep;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec4 sum = texture(uTexture, vUv + vec2(-uStep.x * 2.0, 0.0));
  sum += texture(uTexture, vUv + vec2(-uStep.x, uStep.y)) * 2.0;
  sum += texture(uTexture, vUv + vec2(0.0, uStep.y * 2.0));
  sum += texture(uTexture, vUv + vec2(uStep.x, uStep.y)) * 2.0;
  sum += texture(uTexture, vUv + vec2(uStep.x * 2.0, 0.0));
  sum += texture(uTexture, vUv + vec2(uStep.x, -uStep.y)) * 2.0;
  sum += texture(uTexture, vUv + vec2(0.0, -uStep.y * 2.0));
  sum += texture(uTexture, vUv + vec2(-uStep.x, -uStep.y)) * 2.0;
  fragColor = sum / 12.0;
}
)glsl";
```

After `struct CompositeProgram`:

```cpp
struct BlurProgram {
  unsigned id{};
  int step{-1};
};

struct BlurChain {
  std::array<Target, game::kMaxBlurLevels> levels{};
  int count{};
};
```

Add to `State`, after `Target fogCapture{};`:

```cpp
  BlurProgram blurDown{};
  BlurProgram blurUp{};
  BlurChain fogChain{};
```

After `create_target`:

```cpp
void destroy_chain(BlurChain& chain) noexcept {
  for (auto& level : chain.levels) destroy_target(level);
  chain.count = 0;
}

bool create_chain(BlurChain& chain, game::Extent base) noexcept {
  destroy_chain(chain);
  const int count = game::available_blur_levels(base);
  game::Extent extent = base;
  for (int index = 0; index < count; ++index) {
    extent = game::half_extent(extent);
    if (!create_target(chain.levels[static_cast<std::size_t>(index)], extent, gl::RGBA16F,
                       gl::HALF_FLOAT)) {
      destroy_chain(chain);
      return false;
    }
  }
  chain.count = count;
  return true;
}

void blur_pass(const BlurProgram& program, const Target& source, const Target& destination,
               float stepTexels) noexcept {
  const auto& fn = gl::get_gl_functions();
  fn.glBindFramebuffer(gl::FRAMEBUFFER, destination.framebuffer);
  fn.glViewport(0, 0, destination.extent.width, destination.extent.height);
  fn.glUseProgram(program.id);
  fn.glUniform2f(program.step, stepTexels / static_cast<float>(source.extent.width),
                 stepTexels / static_cast<float>(source.extent.height));
  fn.glBindTexture(gl::TEXTURE_2D, source.texture);
  fn.glDrawArrays(gl::TRIANGLES, 0, 3);
}

// Blurs `source` into chain.levels[0]. With `accumulate`, each level keeps its
// own downsampled content and the wider blur is added on top (bloom); without
// it the result is a plain blur (fog). Requires set_pass_state().
void run_blur(const Target& source, BlurChain& chain, game::BlurPlan plan,
              bool accumulate) noexcept {
  const auto& fn = gl::get_gl_functions();
  const int levels = plan.levels < chain.count ? plan.levels : chain.count;
  if (levels <= 0) return;
  fn.glDisable(gl::BLEND);
  const Target* from = &source;
  for (int index = 0; index < levels; ++index) {
    const auto& to = chain.levels[static_cast<std::size_t>(index)];
    blur_pass(g_state.blurDown, *from, to, plan.offset);
    from = &to;
  }
  if (accumulate) {
    fn.glEnable(gl::BLEND);
    fn.glBlendFunc(gl::ONE, gl::ONE);
  }
  for (int index = levels - 1; index > 0; --index) {
    blur_pass(g_state.blurUp, chain.levels[static_cast<std::size_t>(index)],
              chain.levels[static_cast<std::size_t>(index - 1)], plan.offset * 0.5f);
  }
  fn.glDisable(gl::BLEND);
}
```

In `create_shared_resources`, before `return true;`:

```cpp
  const auto build_blur = [&](BlurProgram& program, const char* source) {
    program.id = build_program(source);
    if (!program.id) return false;
    program.step = fn.glGetUniformLocation(program.id, "uStep");
    fn.glUseProgram(program.id);
    fn.glUniform1i(fn.glGetUniformLocation(program.id, "uTexture"), 0);
    return true;
  };
  if (!build_blur(g_state.blurDown, kBlurDownSource)) return false;
  if (!build_blur(g_state.blurUp, kBlurUpSource)) return false;
```

In `create_sized_resources`, after the fog capture is created:

```cpp
  if (!create_chain(g_state.fogChain, viewport)) return false;
```

- [ ] **Step 2: Use the blur in the fog composite**

Replace the body of `world_post_after_fog` from `bind_engine_framebuffer();` through the `draw_composite(...)` call with:

```cpp
  set_pass_state();
  const auto plan = game::blur_plan_for_radius(g_state.fogRadius * g_state.pixelsPerWorldPixel,
                                               g_state.viewport);
  const bool blurred = plan.levels > 0 && g_state.fogChain.count > 0;
  if (blurred) run_blur(g_state.fogCapture, g_state.fogChain, plan, false);
  bind_engine_framebuffer();
  fn.glViewport(0, 0, g_state.viewport.width, g_state.viewport.height);
  // framebuffer = framebuffer * visible fraction
  fn.glEnable(gl::BLEND);
  fn.glBlendFunc(gl::DST_COLOR, gl::ZERO);
  draw_composite(blurred ? g_state.fogChain.levels[0].texture : g_state.fogCapture.texture, 1.0f,
                 blurred ? 1.0f : 0.0f);
```

Extend the once-per-area `LOG_INFO` in `world_post_before_fog` so the plan is visible: compute `const auto plan = game::blur_plan_for_radius(g_state.fogRadius * g_state.pixelsPerWorldPixel, {viewport[2], viewport[3]});` just before the log and append `, fog blur levels={} offset={:.2f}` with `plan.levels, plan.offset`.

- [ ] **Step 3: Build**

Run: `cmake --build build-host --target iee_tests && ctest --test-dir build-host --output-on-failure && cmake --build build-mingw`
Expected: tests pass; mingw builds with no warnings.

- [ ] **Step 4: Commit and push**

```bash
git add src/iee/features/world_post.cpp
git commit -m "World post: soft fog of war edge"
git push
```

- [ ] **Step 5: Owner in-game check (blocking)**

INI: `SoftFogOfWar = true`, `SoftFogRadius = 24`.

1. The fog edge is a smooth gradient with no diamond or stair-step shapes.
2. Zoom fully in and fully out, changing area or reloading between them so the log line prints again: the logged `px per world px` differs between the two, and the edge softness looks proportionate at both. (If the value does not change with zoom, the view transform is stale at this point of the frame; read the zoom from `CInfinity::m_fZoom` at `+0x484` on the hook's `infinity` argument instead, via a manifest offset.)
3. No banding in the dark gradient; no light leaking in from unexplored areas beyond a few pixels.
4. `SoftFogRadius = 0` still matches vanilla.
5. Try 12 and 48 and report the preferred value; update the default in `config.h`, the Task 3 test and the spec if it is not 24.

---

### Task 8: Bloom

**Files:**
- Modify: `src/iee/features/world_post.cpp`

**Interfaces:**
- Consumes: `BlurChain`, `create_chain`, `destroy_chain`, `run_blur`, `draw_composite`, `set_pass_state`, `bind_engine_framebuffer` (Tasks 6–7); `game::bloom_levels`, `game::half_extent` (Task 2).
- Produces: bloom drawn inside `world_post_before_fog`; no interface change for callers.

- [ ] **Step 1: Add the bright-pass program and bloom targets**

After `kBlurUpSource`:

```cpp
// Keeps what is brighter than uThreshold, with a soft knee so the cut-off
// does not flicker.
constexpr const char* kBrightSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform float uThreshold;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec3 color = texture(uTexture, vUv).rgb;
  float level = max(color.r, max(color.g, color.b));
  const float knee = 0.1;
  float soft = clamp(level - uThreshold + knee, 0.0, 2.0 * knee);
  soft = soft * soft / (4.0 * knee);
  float weight = max(soft, level - uThreshold) / max(level, 1e-4);
  fragColor = vec4(color * weight, 1.0);
}
)glsl";
```

After `struct BlurProgram`:

```cpp
struct BrightProgram {
  unsigned id{};
  int threshold{-1};
};
```

Add to `State`, after `BlurChain fogChain{};`:

```cpp
  BrightProgram bright{};
  Target bloomScene{};   // half-size copy of the world
  Target bloomBright{};  // its bright parts
  BlurChain bloomChain{};
  bool bloomDrawnOnce{};
```

In `create_shared_resources`, before `return true;`:

```cpp
  g_state.bright.id = build_program(kBrightSource);
  if (!g_state.bright.id) return false;
  g_state.bright.threshold = fn.glGetUniformLocation(g_state.bright.id, "uThreshold");
  fn.glUseProgram(g_state.bright.id);
  fn.glUniform1i(fn.glGetUniformLocation(g_state.bright.id, "uTexture"), 0);
```

In `create_sized_resources`, after the fog chain:

```cpp
  const auto half = game::half_extent(viewport);
  if (!create_target(g_state.bloomScene, half, gl::RGBA16F, gl::HALF_FLOAT)) return false;
  if (!create_target(g_state.bloomBright, half, gl::RGBA16F, gl::HALF_FLOAT)) return false;
  if (!create_chain(g_state.bloomChain, half)) return false;
```

- [ ] **Step 2: Add the bloom pass**

After `run_blur`:

```cpp
// Adds a glow of the bright parts of the world image to the engine's
// framebuffer. Runs before the fog so unexplored areas stay dark, and before
// the UI is drawn so the UI cannot glow. Requires set_pass_state().
void draw_bloom() noexcept {
  const auto& fn = gl::get_gl_functions();
  const auto half = g_state.bloomScene.extent;
  const int levels = game::bloom_levels(half);
  if (levels <= 0 || !(g_state.bloomStrength > 0.0f)) return;

  fn.glBindFramebuffer(gl::READ_FRAMEBUFFER,
                       static_cast<unsigned>(g_state.engineDrawFramebuffer));
  fn.glBindFramebuffer(gl::DRAW_FRAMEBUFFER, g_state.bloomScene.framebuffer);
  fn.glBlitFramebuffer(0, 0, g_state.viewport.width, g_state.viewport.height, 0, 0, half.width,
                       half.height, gl::COLOR_BUFFER_BIT, gl::LINEAR);

  fn.glBindFramebuffer(gl::FRAMEBUFFER, g_state.bloomBright.framebuffer);
  fn.glViewport(0, 0, half.width, half.height);
  fn.glUseProgram(g_state.bright.id);
  fn.glUniform1f(g_state.bright.threshold, g_state.bloomThreshold);
  fn.glBindTexture(gl::TEXTURE_2D, g_state.bloomScene.texture);
  fn.glDrawArrays(gl::TRIANGLES, 0, 3);

  run_blur(g_state.bloomBright, g_state.bloomChain, {levels, 1.0f}, true);

  bind_engine_framebuffer();
  fn.glViewport(0, 0, g_state.viewport.width, g_state.viewport.height);
  fn.glEnable(gl::BLEND);
  fn.glBlendFunc(gl::ONE, gl::ONE);
  // Each level adds its share on the way up; normalise so strength means the
  // same thing at every resolution.
  draw_composite(g_state.bloomChain.levels[0].texture,
                 g_state.bloomStrength / static_cast<float>(levels), 0.0f);
  fn.glDisable(gl::BLEND);

  if (!g_state.bloomDrawnOnce) {
    g_state.bloomDrawnOnce = true;
    if (!gl::check_error("world post bloom")) {
      fail("the bloom pass raised a GL error");
    } else {
      try {
        LOG_INFO("World post: first bloom drawn ({} levels)", levels);
      } catch (...) {
      }
    }
  }
}
```

In `world_post_before_fog`, right after `set_pass_state();` and before the `if (g_state.softFog)` block:

```cpp
      if (g_state.bloom) {
        draw_bloom();
        set_pass_state();
      }
```

(`set_pass_state()` is repeated because `draw_bloom` leaves blending and bindings in its own state; the fog clear that follows needs the baseline.)

A multisampled default framebuffer makes the scaled `glBlitFramebuffer` raise `GL_INVALID_OPERATION`; the first-frame error check then latches the feature off with a log line, which is the intended fail-closed behaviour.

- [ ] **Step 3: Build**

Run: `cmake --build build-host --target iee_tests && ctest --test-dir build-host --output-on-failure && cmake --build build-mingw`
Expected: tests pass; mingw builds with no warnings.

- [ ] **Step 4: Commit and push**

```bash
git add src/iee/features/world_post.cpp
git commit -m "World post: threshold bloom on the world image"
git push
```

- [ ] **Step 5: Owner in-game check (blocking)**

INI: `Bloom = true`, defaults otherwise; test with `SoftFogOfWar` both on and off.

1. Log shows `first bloom drawn`; no `disabled for this session`.
2. Fires, torches, spell effects and lit windows glow.
3. UI panels, text, portraits and the cursor do not glow.
4. Unexplored (black) areas show no glow leaking over them.
5. A bright daytime outdoor map and a snow map are not hazy at the defaults.
6. With `Bloom = true` and `SoftFogOfWar = false` the fog looks vanilla (this is the bloom-only path: one flush, engine fog untouched).
7. Tune `BloomThreshold` (try 0.7 and 0.9) and `BloomStrength` (try 0.2 and 0.6); report preferred values and update the defaults in `config.h`, the Task 3 test and the spec if they differ.

---

### Task 9: Runtime toggle and documentation

**Files:**
- Modify: `src/iee/features/world_post.h`, `src/iee/features/world_post.cpp`, `src/iee/hooks.cpp`
- Modify: `docs/superpowers/specs/2026-10-03-graphics-uplift-roadmap-design.md` (items D1/D2, §3 order), `docs/superpowers/specs/2026-06-10-graphics-enhancement-roadmap-design.md` (note on phases 4–5), `CLAUDE.md` (Runtime Facts — Renderer / GL), `../handoff.md`

**Interfaces:**
- Produces: `void features::world_post_poll_hotkeys() noexcept;` — render thread; F8 toggles soft fog, F9 toggles bloom.

- [ ] **Step 1: Add the hotkey poll**

`world_post.h`, after `world_post_active`:

```cpp
// Debug A/B: F8 toggles soft fog of war, F9 toggles bloom. Call once per
// frame, only when debug hotkeys are enabled.
void world_post_poll_hotkeys() noexcept;
```

`world_post.cpp`, after `world_post_active`:

```cpp
void world_post_poll_hotkeys() noexcept {
  static bool fogKeyWasDown = false;
  static bool bloomKeyWasDown = false;
  const bool fogKeyDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
  const bool bloomKeyDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
  const bool fogPressed = fogKeyDown && !fogKeyWasDown;
  const bool bloomPressed = bloomKeyDown && !bloomKeyWasDown;
  fogKeyWasDown = fogKeyDown;
  bloomKeyWasDown = bloomKeyDown;
  if (!fogPressed && !bloomPressed) return;
  if (fogPressed) g_state.softFog = !g_state.softFog;
  if (bloomPressed) g_state.bloom = !g_state.bloom;
  try {
    LOG_INFO("Hotkey: soft fog of war={}, bloom={}", g_state.softFog, g_state.bloom);
  } catch (...) {
  }
}
```

`hooks.cpp`, as the first statements of `detour_render_fog` after `const auto original = ...;`:

```cpp
  if (g_ctx && g_ctx->cfg.enableDebugHotkeys) features::world_post_poll_hotkeys();
```

- [ ] **Step 2: Build**

Run: `cmake --build build-host --target iee_tests && ctest --test-dir build-host --output-on-failure && cmake --build build-mingw`
Expected: tests pass; mingw builds with no warnings.

- [ ] **Step 3: Update the documents**

- `CLAUDE.md`, under "Runtime Facts — Renderer / GL", add:

```markdown
- The engine queues every draw (`DrawEnd_GL` only appends to `gl.cmds`) and submits the whole frame in `DrawFlush_GL` (2.7.3 RVA `0x42B350`), normally once at flip. Binding a framebuffer around an engine render function captures nothing, and an immediate GL draw of ours lands under the still-queued world. Flush first (see `features/world_post.*`), and restore every GL state touched: the flush issues changes relative to the engine's cached `gl.hwState`.
- `gl.pp.enabled` is forced false in 2.7.3 (`DrawInit_GL`): the engine's own offscreen target and `fpCatRom` are dead.
```

- `2026-10-03-graphics-uplift-roadmap-design.md`: in item D, mark D1 and D2 implemented behind `[Rendering] SoftFogOfWar` / `Bloom` with a pointer to the 2026-10-07 spec; in §3, move D1/D2 to "Done" and note that F's pack work is paused by owner decision 2026-10-07.
- `2026-06-10-graphics-enhancement-roadmap-design.md`: at the top of "Phase 4 — Fog" add one line: "Superseded by `2026-10-07-soft-fog-and-bloom-design.md`: the engine queues draws, so an FBO bracket around `RenderFog` captures nothing."
- `../handoff.md`: update the date, the State table (soft fog and bloom rows with their in-game status; animation in-betweens pack marked paused), replace "Phase 1" as next step with effect-cast light, and add the queue/flush fact to Working notes.

- [ ] **Step 4: Commit and push**

```bash
git add src/iee/features/world_post.h src/iee/features/world_post.cpp src/iee/hooks.cpp CLAUDE.md docs/superpowers/specs
git commit -m "World post: debug A/B hotkeys; record the engine draw queue in the docs"
git push
```

(`../handoff.md` lives outside the repository; edit it in place.)

- [ ] **Step 5: Final verification**

Run: `cmake --build build-host --target iee_tests && ctest --test-dir build-host --output-on-failure && cmake --build build-mingw`
Expected: tests pass; mingw builds with no warnings. Confirm CI is green on the PR, and that with both settings `false` and hotkeys off the log shows no `RenderFog hook installed` line (addresses are not even resolved).
