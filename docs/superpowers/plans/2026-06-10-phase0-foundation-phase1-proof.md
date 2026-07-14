# Phase 0 (Foundation) + Phase 1 (Proof of Pipeline) Implementation Plan

> **Status: Historical implementation plan. Do not execute as a current
> runbook.** It records the intended Phase 0/1 sequence, including experiments
> and branch commands that were later superseded. The production module model
> is documented in [`docs/architecture.md`](../../architecture.md); live gate
> outcomes are in
> [`docs/validation/phase0-gates.md`](../../validation/phase0-gates.md).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the shader-override and GL infrastructure layer (Phase 0) and prove it end-to-end by replacing the engine's `fpSELECT` shader, plus run validation gates V1/V2/V3/V5 (Phase 1).

**Architecture:** Host-safe logic (shader name extraction, override registry, area-texture packing, config) lives in `iee_common` with WSL/macOS-testable unit tests. Windows-only code (MinHook detours on GL entry points, SDL swap hook, GL state guard) lives in the DLL target and is verified by the CI Windows job plus in-game logging runbooks. Engine shaders are replaced wholesale at `glShaderSource` time, identified by their `// <name>.glsl` comment; per-frame uniforms are fed at `glUseProgram` time (never per-tile — see spec §4.1).

**Tech Stack:** C++20, CMake, MinHook, spdlog, OpenGL 4.6 compatibility profile, GLSL `#version 460 compatibility`.

**Spec:** `docs/superpowers/specs/2026-06-10-graphics-enhancement-roadmap-design.md`

**Standing rules for every task:**
- Commit messages: plain, no Co-Authored-By trailer.
- Host tests are the only locally runnable verification (macOS/WSL). Windows-only code is compile-verified by pushing and checking the CI Windows job: `git push && gh run watch --exit-status $(gh run list --limit 1 --json databaseId --jq '.[0].databaseId')`.
- In-game validation steps produce log evidence; paste the relevant log lines into `docs/validation/phase0-gates.md` as you go.
- Never detour `CVisibilityMap::BltFogOWar3d` (confirmed crasher). Not used in this plan — listed as a guardrail.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/iee/core/config.h/.cpp` | modify | New Phase 0/1 INI flags |
| `src/iee/game/build_manifest.h/.cpp` | modify | CInfGame area offsets + CInfinity zoom offset move into manifest |
| `src/iee/game/opengl_types.h/.cpp` | modify | Port WIP GL loader extension + GL1.1 upload functions + FBO/probe functions |
| `src/iee/game/shader_override.h/.cpp` | create | **Host-safe**: shader name extraction, override registry (load `.glsl` files from disk), magenta patch, contract check |
| `src/iee/game/area_texture.h/.cpp` | create | **Host-safe**: pack `WedAreaInfo.baseOverlayFlags` into an R8 grid |
| `src/iee/shader_probe.h/.cpp` | create | Windows: MinHook detours on `glShaderSource/glCompileShader/glLinkProgram/glUseProgram` (+ARB), original-source archival, override substitution, per-frame uniform feed |
| `src/iee/frame_hook.h/.cpp` | create | Windows: `SDL_GL_SwapWindow` hook, frame-tick callback registry, `glBindFramebuffer` V5 probe, hotkey polling |
| `src/iee/core/gl_state_guard.h` | create | Windows: RAII save/restore of program, active texture unit, texture bindings |
| `src/iee/area_state.h/.cpp` | create | Windows: `resolve_active_area`, `read_area_scroll`, `refresh_wed_cache` extracted from hooks.cpp + area-texture upload |
| `src/iee/diagnostics.h/.cpp` | create | Windows: `log_pointer_dwords`, `log_tis_header_diagnostics` extracted from hooks.cpp |
| `src/iee/features/tile_render.h/.cpp` | create | Windows: tile-upscale render path + its state (extracted from hooks.cpp/AppContext) |
| `src/iee/hooks.cpp` | modify | Shrinks to thin detour → dispatch |
| `src/iee/app_context.h` | modify | Loses per-feature atomics; keeps shared infra |
| `src/iee/dll_main.cpp` | modify | Probe/frame-hook lifecycle wiring |
| `assets/shaders/fpSELECT.glsl` | create | Phase 1: replacement fragment shader |
| `assets/shaders/fpCatRom.glsl` | create | Phase 1 (conditional on V3): bicubic replacement |
| `tests/iee_tests.cpp` | modify | New host tests appended (existing expect_true/expect_eq style) |
| `CMakeLists.txt` | modify | New sources; bundle shader assets into release_bundle |
| `docs/validation/phase0-gates.md` | create | Gate evidence record (V1/V2/V3/V5) |

Branch: create `feat/phase0-foundation` from `main` before Task 1 commits.

---

### Task 1: Local host toolchain + baseline

**Files:** none (environment)

- [ ] **Step 1: Install CMake (macOS has none)**

```bash
brew install cmake
cmake --version   # expect >= 3.20
```

If on WSL instead: `sudo apt-get install -y cmake g++`.

- [ ] **Step 2: Create the branch**

```bash
git checkout main && git pull && git checkout -b feat/phase0-foundation
```

- [ ] **Step 3: Baseline host build + tests**

```bash
cmake -S . -B cmake-build-debug -DBUILD_TESTING=ON
cmake --build cmake-build-debug --target iee_tests
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: configure fetches spdlog, build succeeds, `iee_tests ... Passed`. If Apple clang errors on existing code, stop and report — do not "fix" unrelated code.

---

### Task 2: Config flags

**Files:**
- Modify: `src/iee/core/config.h`
- Modify: `src/iee/core/config.cpp`
- Test: `tests/iee_tests.cpp`

- [ ] **Step 1: Add fields to `EngineConfig`** (`src/iee/core/config.h`, inside the struct, before `enableVerboseLogging`):

```cpp
        // region Shader overrides (Phase 0)
        bool enableShaderOverrides = false;     // master switch for source replacement
        bool dumpEngineShaders = true;          // archive originals to <dll dir>/iee-shader-dumps/
        std::string shaderOverrideDir = "iee-shaders"; // relative to DLL dir
        std::string debugMagentaShaders;        // comma-separated fp names to flood magenta
        bool enableDebugHotkeys = false;        // F10 toggles uIeeEnabled on overridden programs
        // endregion
```

- [ ] **Step 2: Write the failing test** (append to `tests/iee_tests.cpp` before `int main()`, and call it from `main`):

```cpp
void test_config_shader_override_defaults() {
    iee::core::EngineConfig cfg{};
    expect_true(!cfg.enableShaderOverrides, "shader overrides default off");
    expect_true(cfg.dumpEngineShaders, "shader dump defaults on");
    expect_eq(cfg.shaderOverrideDir, std::string("iee-shaders"), "override dir default");
    expect_true(cfg.debugMagentaShaders.empty(), "magenta list default empty");
    expect_true(!cfg.enableDebugHotkeys, "hotkeys default off");
}
```

- [ ] **Step 3: Run** `cmake --build cmake-build-debug --target iee_tests && ctest --test-dir cmake-build-debug --output-on-failure` — expect FAIL (fields missing) before Step 1 is applied, PASS after. If you applied Step 1 first, just confirm PASS.

- [ ] **Step 4: Wire INI parse/save** in `src/iee/core/config.cpp`. Follow the existing `iequals(section, ...)` dispatch pattern; add a `[Shaders]` section in the parse function:

```cpp
        // [Shaders]
        if (iequals(section, "shaders")) {
            if (iequals(key, "EnableOverrides")) cfg.enableShaderOverrides = parse_bool(val, cfg.enableShaderOverrides);
            else if (iequals(key, "DumpEngineShaders")) cfg.dumpEngineShaders = parse_bool(val, cfg.dumpEngineShaders);
            else if (iequals(key, "OverrideDir")) cfg.shaderOverrideDir = val;
            else if (iequals(key, "MagentaShaders")) cfg.debugMagentaShaders = val;
            else if (iequals(key, "EnableDebugHotkeys")) cfg.enableDebugHotkeys = parse_bool(val, cfg.enableDebugHotkeys);
            return;
        }
```

(Use the file's existing bool-parsing helper; if it is named differently, e.g. `to_bool`, match it.) Mirror in `save()` with a `write_section(f, "Shaders")` block writing all five keys.

- [ ] **Step 5: Add a parse round-trip test** (same pattern as any existing config test in the file; if none exists, test via `ConfigManager::save` to a temp path + `ConfigManager::load` and assert the five fields round-trip). Run tests — expect PASS.

- [ ] **Step 6: Commit** — `git add -A && git commit -m "config: add shader override and debug hotkey settings"`

---

### Task 3: Manifest compliance — area offsets + zoom offset

**Files:**
- Modify: `src/iee/game/build_manifest.h` (RuntimeOffsets), `src/iee/game/build_manifest.cpp` (BGEE manifest values)
- Modify: `src/iee/hooks.cpp` (consume manifest instead of local constants)
- Test: `tests/iee_tests.cpp`

- [ ] **Step 1: Extend `RuntimeOffsets`** in `build_manifest.h`:

```cpp
    struct RuntimeOffsets {
        std::uintptr_t vidTileResource{};
        std::uintptr_t tisLinearTilesFlag{};
        std::uintptr_t tisHeaderTileDimension{};
        std::uintptr_t infGameVisibleArea{};   // CInfGame: current visible area index (u8)
        std::uintptr_t infGameAreas{};         // CInfGame: CGameArea*[12]
        std::uintptr_t infGameAreaMaster{};    // CInfGame: master area pointer
        std::uintptr_t infinityZoom{};         // CInfinity: float m_fZoom (EEex docs +0x484)
    };
```

Extend `validate()` to require the four new fields non-zero.

- [ ] **Step 2: Set values in the BGEE manifest** (`build_manifest.cpp`, in the existing `offsets` initializer):

```cpp
        .infGameVisibleArea = 0x6590,
        .infGameAreas = 0x6598,
        .infGameAreaMaster = 0x65F8,
        .infinityZoom = 0x484,
```

- [ ] **Step 3: Write the failing test** (append + call from main):

```cpp
void test_manifest_infgame_offsets() {
    const auto &m = iee::game::current_manifest();
    expect_eq(m.offsets.infGameVisibleArea, std::uintptr_t{0x6590}, "visible area offset");
    expect_eq(m.offsets.infGameAreas, std::uintptr_t{0x6598}, "areas array offset");
    expect_eq(m.offsets.infGameAreaMaster, std::uintptr_t{0x65F8}, "master area offset");
    expect_eq(m.offsets.infinityZoom, std::uintptr_t{0x484}, "CInfinity zoom offset");
    expect_true(m.validate(), "manifest still validates");
}
```

- [ ] **Step 4: Run host tests** — expect PASS.

- [ ] **Step 5: Consume in `hooks.cpp`**: delete `kInfGameVisibleAreaOffset/kInfGameAreasOffset/kInfGameAreaMasterOffset` (hooks.cpp:43-45) and change `resolve_active_area` to take the manifest:

```cpp
        const game::CGameArea *resolve_active_area(void *infGame, const game::BuildManifest &manifest) {
            ...
            if (!core::safe_read(gameBytes + manifest.offsets.infGameVisibleArea, visibleArea) ||
                !core::safe_read(gameBytes + manifest.offsets.infGameAreas, areas) ||
```

Update the one caller (`refresh_wed_cache`) to pass `*ctx.manifest`. This file is Windows-only — no local compile; CI verifies in Step 6.

- [ ] **Step 6: Commit + CI** — `git add -A && git commit -m "manifest: move CInfGame area offsets and CInfinity zoom into build manifest" && git push -u origin feat/phase0-foundation` then watch the run (`gh run watch --exit-status ...`). Expected: both host and Windows jobs green.

---

### Task 4: GL function loader extension

**Files:**
- Modify: `src/iee/game/opengl_types.h`, `src/iee/game/opengl_types.cpp`

The WIP branch already wrote most of this. Port it, then extend.

- [ ] **Step 1: Port the WIP loader**

```bash
git diff main origin/feature/wip -- src/iee/game/opengl_types.h src/iee/game/opengl_types.cpp > /tmp/glloader.patch
git apply /tmp/glloader.patch
```

If the apply fails on context drift, apply the hunks manually — the full WIP versions are readable via `git show origin/feature/wip:src/iee/game/opengl_types.h`.

- [ ] **Step 2: Extend with upload/probe/uniform functions** (add to the typedef region, struct, and the loader in `opengl_types.cpp`):

```cpp
// GL1.1 exports — load via GetProcAddress(GetModuleHandleA("opengl32.dll"), name),
// NOT wglGetProcAddress (which returns null for GL1.1 core entry points):
using PFN_glGenTextures = void (APIENTRY*)(int n, unsigned* textures);
using PFN_glBindTexture = void (APIENTRY*)(unsigned target, unsigned texture);
using PFN_glTexImage2D = void (APIENTRY*)(unsigned target, int level, int internalformat,
                                          int width, int height, int border,
                                          unsigned format, unsigned type, const void* data);
using PFN_glDeleteTextures = void (APIENTRY*)(int n, const unsigned* textures);
using PFN_glPixelStorei = void (APIENTRY*)(unsigned pname, int param);
// Extensions — wglGetProcAddress:
using PFN_glActiveTexture = void (APIENTRY*)(unsigned texture);
using PFN_glBindFramebuffer = void (APIENTRY*)(unsigned target, unsigned framebuffer);
using PFN_glUniform2f = void (APIENTRY*)(int location, float v0, float v1);

// Constants:
constexpr unsigned TEXTURE0 = 0x84C0;
constexpr unsigned R8 = 0x8229;
constexpr unsigned RED = 0x1903;
constexpr unsigned UNSIGNED_BYTE = 0x1401;
constexpr unsigned UNPACK_ALIGNMENT = 0x0CF5;
constexpr unsigned FRAMEBUFFER = 0x8D40;
```

In `initialize()`, load the GL1.1 group via `GetProcAddress` on the `opengl32.dll` module handle and the rest via the existing `wglGetProcAddress` path; add a `bool textureUploadAvailable` capability flag set when all five GL1.1 pointers plus `glActiveTexture` resolved.

- [ ] **Step 3: Commit + CI push.** `git add -A && git commit -m "gl: port shader/program loader from wip and add upload, uniform, fbo-probe functions"` — push; Windows job must compile. (This file is in the DLL target only; host job is unaffected.)

---

### Task 5: Shader override engine (host-safe, TDD-heavy)

**Files:**
- Create: `src/iee/game/shader_override.h`, `src/iee/game/shader_override.cpp`
- Modify: `CMakeLists.txt` (add to `iee_common` sources)
- Test: `tests/iee_tests.cpp`

- [ ] **Step 1: Write the header** (`src/iee/game/shader_override.h`):

```cpp
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iee::game {
    // Extracts the engine's shader name from a source preview.
    // Engine convention: a comment line "// <name>.glsl" near the top.
    // `prefix` filters by kind ("fp" or "vp"); empty accepts both.
    [[nodiscard]] std::string extract_shader_name(std::string_view source, std::string_view prefix);

    // Finds the offset of '{' opening main()'s body, or npos.
    [[nodiscard]] std::size_t find_main_body_open(std::string_view source);

    // Builds the magenta debug variant of a fragment source (inserts an early
    // return of vec4(1,0,1,1) at the top of main). Returns empty if not patchable.
    [[nodiscard]] std::string make_magenta_variant(std::string_view source);

    struct OverrideCheck {
        bool ok{};
        std::vector<std::string> missingIdentifiers;
    };
    // Contract check: every uniform/varying identifier declared in the original
    // must appear in the replacement (the engine queries them by name; see spec §4.2).
    [[nodiscard]] OverrideCheck check_interface_contract(std::string_view originalSource,
                                                         std::string_view replacementSource);

    class ShaderOverrideRegistry {
    public:
        // Loads every "<name>.glsl" in dir (non-recursive). Missing dir => empty registry.
        void load_from_directory(const std::filesystem::path &dir);
        [[nodiscard]] std::optional<std::string_view> find(std::string_view shaderName) const;
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    private:
        std::unordered_map<std::string, std::string> entries_; // name -> source
    };
}
```

- [ ] **Step 2: Write the failing tests** (append to `tests/iee_tests.cpp`; call all from `main`):

```cpp
void test_shader_name_extraction() {
    using iee::game::extract_shader_name;
    expect_eq(extract_shader_name("// fpSEAM.glsl\nuniform float uTcScale;", "fp"),
              std::string("fpSEAM"), "extracts fp name");
    expect_eq(extract_shader_name("// vpDraw.glsl\nvoid main(){}", "vp"),
              std::string("vpDraw"), "extracts vp name");
    expect_true(extract_shader_name("// vpDraw.glsl\n", "fp").empty(), "prefix filter rejects vp");
    expect_true(extract_shader_name("no comment here", "fp").empty(), "no name -> empty");
}

void test_find_main_body_open() {
    using iee::game::find_main_body_open;
    const std::string_view src = "uniform float x;\nvoid main() {\n  gl_FragColor = vec4(x);\n}";
    const auto pos = find_main_body_open(src);
    expect_true(pos != std::string_view::npos, "finds main body");
    expect_eq(src[pos], '{', "offset points at brace");
    expect_true(find_main_body_open("float f(){}") == std::string_view::npos, "no main -> npos");
}

void test_magenta_variant() {
    const std::string_view src = "// fpSELECT.glsl\nvoid main() {\n  gl_FragColor = vec4(1.0);\n}";
    const auto patched = iee::game::make_magenta_variant(src);
    expect_true(!patched.empty(), "patchable source produces variant");
    expect_true(patched.find("vec4(1.0, 0.0, 1.0, 1.0)") != std::string::npos, "magenta color present");
    expect_true(patched.find("IEE_DEBUG_MAGENTA") != std::string::npos, "marker present");
    expect_true(iee::game::make_magenta_variant("no main here").empty(), "unpatchable -> empty");
}

void test_interface_contract() {
    const std::string_view original =
        "// fpSEAM.glsl\nuniform sampler2D sTex;\nuniform float uTcScale;\nvarying vec2 vTc;\nvoid main(){}";
    const std::string_view good =
        "#version 460 compatibility\nuniform sampler2D sTex;\nuniform float uTcScale;\nvarying vec2 vTc;\nvoid main(){}";
    const std::string_view bad =
        "#version 460 compatibility\nuniform sampler2D sTex;\nvoid main(){}";
    expect_true(iee::game::check_interface_contract(original, good).ok, "matching interface passes");
    const auto failed = iee::game::check_interface_contract(original, bad);
    expect_true(!failed.ok, "missing identifiers fail");
    expect_eq(failed.missingIdentifiers.size(), std::size_t{2}, "uTcScale and vTc reported");
}

void test_override_registry() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "iee_override_test";
    fs::create_directories(dir);
    { std::ofstream f(dir / "fpSELECT.glsl"); f << "void main(){}"; }
    iee::game::ShaderOverrideRegistry reg;
    reg.load_from_directory(dir);
    expect_eq(reg.size(), std::size_t{1}, "one override loaded");
    expect_true(reg.find("fpSELECT").has_value(), "finds by name");
    expect_true(!reg.find("fpSEAM").has_value(), "unknown name -> nullopt");
    iee::game::ShaderOverrideRegistry empty;
    empty.load_from_directory(dir / "does-not-exist");
    expect_eq(empty.size(), std::size_t{0}, "missing dir -> empty registry");
    fs::remove_all(dir);
}
```

- [ ] **Step 3: Add `src/iee/game/shader_override.cpp` to `iee_common` in CMakeLists.txt, build, run tests** — expect FAIL (unresolved symbols / missing file).

- [ ] **Step 4: Implement** (`src/iee/game/shader_override.cpp`). Port `extract_shader_name` and `find_main_body_open` logic from the WIP (`git show origin/feature/wip:src/iee/game/shader_runtime.cpp` — functions of the same names) but make them standalone; implement the rest:

```cpp
#include "shader_override.h"
#include <fstream>
#include <sstream>

namespace iee::game {
    std::string extract_shader_name(std::string_view source, std::string_view prefix) {
        const auto commentPos = source.find("// ");
        if (commentPos == std::string_view::npos) return {};
        const auto nameStart = commentPos + 3;
        const auto endPos = source.find(".glsl", nameStart);
        if (endPos == std::string_view::npos || endPos <= nameStart) return {};
        const auto name = source.substr(nameStart, endPos - nameStart);
        if (!prefix.empty() && name.rfind(prefix, 0) != 0) return {};
        return std::string(name);
    }

    std::size_t find_main_body_open(std::string_view source) {
        for (const std::string_view pattern : {"void main(", "void main ("}) {
            if (const auto pos = source.find(pattern); pos != std::string_view::npos) {
                return source.find('{', pos);
            }
        }
        return std::string_view::npos;
    }

    std::string make_magenta_variant(std::string_view source) {
        const auto bodyOpen = find_main_body_open(source);
        if (bodyOpen == std::string_view::npos) return {};
        std::string patched(source);
        constexpr std::string_view injection =
            "\n/* IEE_DEBUG_MAGENTA */\n"
            "gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n"
            "return;\n";
        patched.insert(bodyOpen + 1, injection);
        return patched;
    }

    namespace {
        // Collect identifiers declared as "uniform ... NAME;" or "varying ... NAME;".
        std::vector<std::string> declared_interface_identifiers(std::string_view source) {
            std::vector<std::string> names;
            std::istringstream stream{std::string(source)};
            std::string line;
            while (std::getline(stream, line)) {
                std::istringstream words(line);
                std::string qualifier;
                words >> qualifier;
                if (qualifier != "uniform" && qualifier != "varying") continue;
                std::string token, last;
                while (words >> token) last = token;
                if (last.empty()) continue;
                if (last.back() == ';') last.pop_back();
                // strip array suffix e.g. name[4]
                if (const auto bracket = last.find('['); bracket != std::string::npos)
                    last.resize(bracket);
                if (!last.empty()) names.push_back(last);
            }
            return names;
        }
    }

    OverrideCheck check_interface_contract(std::string_view originalSource,
                                           std::string_view replacementSource) {
        OverrideCheck result{.ok = true};
        for (const auto &name : declared_interface_identifiers(originalSource)) {
            if (replacementSource.find(name) == std::string_view::npos) {
                result.ok = false;
                result.missingIdentifiers.push_back(name);
            }
        }
        return result;
    }

    void ShaderOverrideRegistry::load_from_directory(const std::filesystem::path &dir) {
        entries_.clear();
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".glsl") continue;
            std::ifstream file(entry.path(), std::ios::binary);
            if (!file) continue;
            std::ostringstream contents;
            contents << file.rdbuf();
            entries_[entry.path().stem().string()] = contents.str();
        }
    }

    std::optional<std::string_view> ShaderOverrideRegistry::find(std::string_view shaderName) const {
        const auto it = entries_.find(std::string(shaderName));
        if (it == entries_.end()) return std::nullopt;
        return std::string_view{it->second};
    }
}
```

- [ ] **Step 5: Build + run tests** — expect all PASS.

- [ ] **Step 6: Commit** — `git add -A && git commit -m "game: add host-safe shader override engine with contract checks"`

---

### Task 6: Shader probe (Windows) — detours, archival, substitution, uniform feed

**Files:**
- Create: `src/iee/shader_probe.h`, `src/iee/shader_probe.cpp`
- Create: `src/iee/core/gl_state_guard.h`
- Modify: `CMakeLists.txt` (DLL target sources), `src/iee/dll_main.cpp`, `src/iee/hooks.cpp`

This ports the WIP's `shader_runtime.cpp` probe (932 lines, working) with five changes. Source of truth for the port: `git show origin/feature/wip:src/iee/game/shader_runtime.cpp > /tmp/wip_shader_runtime.cpp` and `.../shader_runtime.h`.

- [ ] **Step 1: Create `src/iee/core/gl_state_guard.h`:**

```cpp
#pragma once
#include "iee/game/opengl_types.h"

namespace iee::core {
    // Saves current program + active texture unit on construction, restores on destruction.
    // Extend with more state as features need it; every detour touching GL must use this.
    class GlStateGuard {
    public:
        GlStateGuard() noexcept {
            auto &gl = game::gl::get_gl_functions();
            if (gl.glGetIntegerv) {
                gl.glGetIntegerv(0x8B8D /*CURRENT_PROGRAM*/, &program_);
                gl.glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &activeTexture_);
            }
        }
        ~GlStateGuard() noexcept {
            auto &gl = game::gl::get_gl_functions();
            if (gl.glUseProgram && program_ >= 0) gl.glUseProgram(static_cast<unsigned>(program_));
            if (gl.glActiveTexture && activeTexture_ >= 0) gl.glActiveTexture(static_cast<unsigned>(activeTexture_));
        }
        GlStateGuard(const GlStateGuard &) = delete;
        GlStateGuard &operator=(const GlStateGuard &) = delete;
    private:
        int program_{-1};
        int activeTexture_{-1};
    };
}
```

- [ ] **Step 2: Port the probe.** Copy the WIP file to `src/iee/shader_probe.cpp` (namespace `iee::probe`), header `shader_probe.h` mirroring the WIP `shader_runtime.h` API (`detect_shader_runtime_capabilities`, `log_shader_runtime_capabilities`, `install_shader_probes(const core::EngineConfig&)`, `uninstall_shader_probes`). Then apply these five changes:

  1. **Delete** `ShaderRenderContext` / `set_shader_render_context` / `clear_shader_render_context` (the per-tile uniform design — rejected by spec §4.1).
  2. **Replace the magenta patcher** internals with calls to `game::make_magenta_variant` / `game::extract_shader_name` from Task 5 (delete the local duplicates).
  3. **Add override substitution** in `detour_glShaderSource` (both GL and ARB variants), before recording, after assembling the full source string:

```cpp
            // Full source assembly (not just preview) when overrides or dumps are active:
            std::string fullSource = gather_full_source(count, string, length); // join all chunks
            const auto name = game::extract_shader_name(fullSource, "");
            std::string substituted;
            if (!name.empty() && g_cfg.enableShaderOverrides) {
                if (const auto replacement = g_registry.find(name)) {
                    const auto contract = game::check_interface_contract(fullSource, *replacement);
                    if (contract.ok) {
                        substituted = std::string(*replacement);
                        LOG_INFO("Shader override applied: {} ({} bytes)", name, substituted.size());
                    } else {
                        for (const auto &id : contract.missingIdentifiers)
                            LOG_WARN("Override {} missing interface identifier '{}' - falling back to engine source", name, id);
                    }
                }
            }
            if (!substituted.empty()) {
                const char *ptr = substituted.c_str();
                const int len = static_cast<int>(substituted.size());
                g_glShaderSourceHook.original()(shader, 1, &ptr, &len);
                // record + return; compile-status logging in detour_glCompileShader
                // reports GLSL errors; on compile failure we resubmit the original:
                g_pendingFallback[shader] = std::move(fullSource);
                return;
            }
```

  and in `detour_glCompileShader`, after the existing compile-status check: if compile failed AND `g_pendingFallback` has an entry for this shader, resubmit the original source via `original glShaderSource` + recompile once, log `LOG_ERROR("Override for shader {} failed to compile; reverted to engine source", ...)`, and erase the entry. Erase on success too.

  4. **Add original-source archival**: in `detour_glUseProgram`'s existing introspection block (where `read_shader_source_preview` runs), when `g_cfg.dumpEngineShaders` and the shader's name is known and not yet dumped, fetch the **full** source via `glGetShaderSource` into a dynamically sized buffer (`glGetShaderiv(shader, 0x8B88 /*SHADER_SOURCE_LENGTH*/, &len)`) and write to `<dll dir>/iee-shader-dumps/<name>.glsl`. Keep a `std::set<std::string>` of dumped names. This works retroactively for shaders compiled before our hooks existed — it is the guaranteed archival path regardless of gate V2.
  5. **Add the per-frame uniform feed**: module-level

```cpp
            struct FrameUniforms { float timeSeconds; float enabled; };
            std::atomic<float> g_uniformTime{0.0f};
            std::atomic<bool>  g_overridesEnabled{true};   // toggled by hotkey (Task 9)
            std::unordered_map<unsigned, bool> g_overriddenPrograms; // programs containing an overridden shader
```

  In `detour_glUseProgram`, after calling the original: if `program` is in `g_overriddenPrograms`, look up (cache per program) `glGetUniformLocation(program, "uIeeTime")` and `"uIeeEnabled"`, and if >= 0 set them via `glUniform1f`. Mark programs as overridden inside the link detour when an attached shader name had an override applied. Locations cached in a `std::unordered_map<unsigned, std::pair<int,int>>`, invalidated on relink. This is the spec §4.1 bridge: frame-level values only, set when the program is provably bound.

  `g_uniformTime` is advanced by the frame hook (Task 8); until Task 8 lands it stays 0 — harmless.

- [ ] **Step 3: Wire lifecycle.** In `hooks.cpp`, keep the WIP's lazy-install pattern: a `log_shader_runtime_capabilities_once()` helper called at the top of `Detour_RenderTexture` (first render = GL context current) that calls `probe::install_shader_probes(ctx.cfg)`. In `install_shader_probes`, load the registry: `g_registry.load_from_directory(dll_directory() / cfg.shaderOverrideDir)` (derive the DLL dir the same way `ConfigManager::config_path()` does). In `dll_main.cpp` `CleanupHooks()`, call `probe::uninstall_shader_probes()` first.

- [ ] **Step 4: CMake**: add `src/iee/shader_probe.cpp` to the DLL target source list.

- [ ] **Step 5: Commit + CI push.** `git commit -m "probe: port gl shader probe with override substitution, archival, and frame uniform feed"`. Windows job must compile. Expect no behavior change in-game with `EnableOverrides = false` (default).

---

### Task 7: hooks.cpp split + AppContext slim

**Files:**
- Create: `src/iee/area_state.h/.cpp`, `src/iee/diagnostics.h/.cpp`, `src/iee/features/tile_render.h/.cpp`
- Modify: `src/iee/hooks.cpp`, `src/iee/app_context.h`, `CMakeLists.txt`

Pure mechanical extraction — no behavior change. All Windows-only; CI verifies.

- [ ] **Step 1: `src/iee/diagnostics.h/.cpp`** — move `log_pointer_dwords` and `log_tis_header_diagnostics` (hooks.cpp:168-269) verbatim into `namespace iee::diagnostics`, exporting both. hooks.cpp includes and qualifies the two call sites.

- [ ] **Step 2: `src/iee/area_state.h/.cpp`** — move `read_loaded_area_candidate`, `resolve_active_area`, `read_area_scroll`, `refresh_wed_cache` (hooks.cpp:47-165) into `namespace iee::area`. Signatures take `AppContext &` / manifest explicitly (Task 3 already added the manifest parameter). Header:

```cpp
#pragma once
#include "iee/game/build_manifest.h"
#include "iee/game/runtime_types_x64.h"

namespace iee { struct AppContext; }

namespace iee::area {
    const game::CGameArea *resolve_active_area(void *infGame, const game::BuildManifest &manifest);
    bool read_area_scroll(const game::CGameArea *area, int &outOffsetX, int &outOffsetY);
    void refresh_wed_cache(AppContext &ctx, void *infGame);
}
```

- [ ] **Step 3: `src/iee/features/tile_render.h/.cpp`** — move the body of `Detour_RenderTexture` (everything after the null-context guard) into:

```cpp
#pragma once
namespace iee { struct AppContext; }
namespace iee::features {
    struct TileRenderState {
        std::atomic<int> lastTexId{-1};
        std::atomic<int> areaScale{1};
        std::atomic<bool> scaleDetected{false};
        std::atomic<int> detectionCount{0};
        void reset() { lastTexId = -1; areaScale = 1; scaleDetected = false; detectionCount = 0; }
    };
    TileRenderState &tile_render_state();   // module-owned singleton

    using Fn_RenderTexture = void (*)(void *, int, void *, int, int, unsigned long);
    // Returns true if it fully handled the draw; false => caller invokes original.
    bool render_tile(AppContext &ctx, void *vidTile, int texId, void *unused,
                     int x, int y, unsigned long flags, Fn_RenderTexture original);
    // Hook-disable requests surface as a flag the dispatcher reads:
    bool should_disable_render_hook();
    void clear_disable_request();
}
```

Move the four atomics out of `AppContext` into `TileRenderState` (delete them from `app_context.h`; `reset_area_state()` now calls `features::tile_render_state().reset()` — keep `AppContext::reset_area_state` but have hooks.cpp's `Detour_LoadArea` call both). The "disable hook after standard-tiles detection" paths set the flag instead of calling `H_RenderTexture.disable()` directly; the dispatcher in hooks.cpp performs the disable (hook objects stay private to hooks.cpp).

- [ ] **Step 4: Shrink `hooks.cpp`** to: hook objects, `g_ctx`, `g_currentAreaId`, thread-local cache reset, the three thin detours (LoadArea calls `ctx.reset_area_state()` + `features::tile_render_state().reset()` + re-enable + original + `area::refresh_wed_cache`; RenderTexture calls probe-install-once + `features::render_tile`, falls back to original on false, honors disable requests; DrawColorTone passthrough), and install/uninstall. Target ≤ 200 lines.

- [ ] **Step 5: CMake**: add the three new `.cpp` files to the DLL target.

- [ ] **Step 6: Commit + CI push** — `git commit -m "refactor: split hooks.cpp into area_state, diagnostics, and tile_render feature module"`. Windows job green. **In-game regression check (next session you run the game):** load a 4x area and a standard area; the log must still show the same detection lines (`Detected ... from TIS header`) as before the refactor.

---

### Task 8: Area data texture + frame hook (SDL swap, V5 probe, time uniform)

**Files:**
- Create: `src/iee/game/area_texture.h/.cpp` (host-safe) + tests
- Create: `src/iee/frame_hook.h/.cpp` (Windows)
- Modify: `src/iee/area_state.cpp` (upload after WED cache refresh), `src/iee/dll_main.cpp`, `CMakeLists.txt`

- [ ] **Step 1: Write failing host tests for the packer:**

```cpp
void test_area_texture_packing() {
    iee::game::WedAreaInfo wed{};
    wed.baseWidth = 3;
    wed.baseHeight = 2;
    wed.baseOverlayFlags = {0x00, 0x02, 0x00, 0x04, 0x00, 0x06}; // overlay bits per cell
    const auto packed = iee::game::pack_area_cell_texture(wed);
    expect_true(packed.has_value(), "packs valid wed");
    expect_eq(packed->width, 3, "width");
    expect_eq(packed->height, 2, "height");
    expect_eq(packed->texels.size(), std::size_t{6}, "one byte per cell");
    expect_eq(packed->texels[1], std::uint8_t{0x02}, "cell flags preserved");
    iee::game::WedAreaInfo empty{};
    expect_true(!iee::game::pack_area_cell_texture(empty).has_value(), "empty wed -> nullopt");
}

void test_area_texture_packing_size_mismatch() {
    iee::game::WedAreaInfo wed{};
    wed.baseWidth = 4; wed.baseHeight = 4;
    wed.baseOverlayFlags = {0x01}; // wrong size
    expect_true(!iee::game::pack_area_cell_texture(wed).has_value(), "flag/dimension mismatch -> nullopt");
}
```

- [ ] **Step 2: Implement** `src/iee/game/area_texture.h/.cpp`:

```cpp
// area_texture.h
#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "wed_runtime.h"

namespace iee::game {
    struct AreaCellTexture {
        int width{};
        int height{};
        std::vector<std::uint8_t> texels; // R8, one texel per WED base cell, row-major
    };
    [[nodiscard]] std::optional<AreaCellTexture> pack_area_cell_texture(const WedAreaInfo &wed);
}
```

```cpp
// area_texture.cpp
#include "area_texture.h"

namespace iee::game {
    std::optional<AreaCellTexture> pack_area_cell_texture(const WedAreaInfo &wed) {
        const std::size_t expected = std::size_t{wed.baseWidth} * wed.baseHeight;
        if (expected == 0 || wed.baseOverlayFlags.size() != expected) return std::nullopt;
        AreaCellTexture out{.width = wed.baseWidth, .height = wed.baseHeight};
        out.texels = wed.baseOverlayFlags;
        return out;
    }
}
```

Add to `iee_common` sources; build; tests PASS. Commit: `git commit -m "game: add host-safe area cell texture packer"`.

- [ ] **Step 3: Upload on area load** (Windows): at the end of `area::refresh_wed_cache` success path, pack and upload:

```cpp
            if (const auto packed = game::pack_area_cell_texture(*cachedWed)) {
                auto &gl = game::gl::get_gl_functions();
                if (gl.textureUploadAvailable) {
                    core::GlStateGuard guard;
                    static unsigned s_areaTexture = 0;
                    if (s_areaTexture == 0) gl.glGenTextures(1, &s_areaTexture);
                    gl.glActiveTexture(game::gl::TEXTURE0 + 2);   // reserved unit 2
                    gl.glBindTexture(game::gl::TEXTURE_2D, s_areaTexture);
                    gl.glPixelStorei(game::gl::UNPACK_ALIGNMENT, 1);
                    gl.glTexImage2D(game::gl::TEXTURE_2D, 0, game::gl::R8,
                                    packed->width, packed->height, 0,
                                    game::gl::RED, game::gl::UNSIGNED_BYTE, packed->texels.data());
                    gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MIN_FILTER, game::gl::NEAREST);
                    gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MAG_FILTER, game::gl::NEAREST);
                    LOG_INFO("Area cell texture uploaded: {}x{} (unit 2, tex {})",
                             packed->width, packed->height, s_areaTexture);
                }
            }
```

(Add `constexpr unsigned NEAREST = 0x2600;` to opengl_types.h.) **Caveat:** `refresh_wed_cache` runs on the LoadArea thread, which may not own the GL context. If the in-game log later shows GL errors here, move the upload to a pending-work flag consumed by the frame hook (Step 4's tick runs on the render thread). Note this in the code comment.

- [ ] **Step 4: Frame hook** `src/iee/frame_hook.h/.cpp`:

```cpp
// frame_hook.h
#pragma once
namespace iee::frame {
    bool install();                       // hooks SDL_GL_SwapWindow from SDL2.dll exports
    void uninstall() noexcept;
    unsigned long long frame_count() noexcept;
    float seconds_since_install() noexcept;
}
```

```cpp
// frame_hook.cpp — core shape
#include "frame_hook.h"
#include "shader_probe.h"
#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include <windows.h>
#include <atomic>

namespace iee::frame {
    namespace {
        using Fn_SwapWindow = void (*)(void *);
        core::Hook<Fn_SwapWindow> g_swapHook;
        std::atomic<unsigned long long> g_frames{0};
        LARGE_INTEGER g_freq{}, g_start{};

        void detour_swap(void *window) {
            g_frames.fetch_add(1, std::memory_order_relaxed);
            probe::on_frame_tick(seconds_since_install()); // advances uIeeTime, polls hotkeys (Task 9)
            g_swapHook.original()(window);
        }
    }

    bool install() {
        const HMODULE sdl = GetModuleHandleA("SDL2.dll");
        if (!sdl) { LOG_WARN("SDL2.dll not loaded; frame hook unavailable"); return false; }
        const auto target = reinterpret_cast<void *>(GetProcAddress(sdl, "SDL_GL_SwapWindow"));
        if (!target) { LOG_WARN("SDL_GL_SwapWindow export not found"); return false; }
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_start);
        try {
            g_swapHook.create(target, reinterpret_cast<void *>(&detour_swap));
            g_swapHook.enable();
            LOG_INFO("Frame hook installed on SDL_GL_SwapWindow");
            return true;
        } catch (const std::exception &e) {
            LOG_WARN("Frame hook install failed: {}", e.what());
            return false;
        }
    }
    // uninstall/frame_count/seconds_since_install: straightforward
}
```

Add `probe::on_frame_tick(float seconds)` to shader_probe: stores into `g_uniformTime`. Install `frame::install()` from `InitThread` after `hooks::install_all` (the SDL export exists without a GL context — safe at init). `CleanupHooks` calls `frame::uninstall()`.

- [ ] **Step 5: V5 probe.** In `install_shader_probes`, also MinHook `glBindFramebuffer` (if loaded) with a detour that logs **once** per distinct (target, framebuffer != 0) pair: `LOG_WARN("V5: engine bound FBO {} on target 0x{:X}", ...)` then calls original. Zero log lines over a session = V5 pass (engine never uses FBOs).

- [ ] **Step 6: CMake + commit + CI push** — `git commit -m "frame: add SDL swap frame hook, area cell texture upload, and V5 fbo probe"`.

---

### Task 9: Debug hotkeys

**Files:**
- Modify: `src/iee/shader_probe.cpp` (inside `on_frame_tick`)

- [ ] **Step 1: Implement polling** in `probe::on_frame_tick` (runs on render thread, once per frame):

```cpp
    void on_frame_tick(float seconds) {
        g_uniformTime.store(seconds, std::memory_order_relaxed);
        if (!g_cfg.enableDebugHotkeys) return;
        // F10: toggle overrides' visual effect (uIeeEnabled), edge-triggered
        static bool f10WasDown = false;
        const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10Down && !f10WasDown) {
            const bool enabled = !g_overridesEnabled.load();
            g_overridesEnabled.store(enabled);
            LOG_INFO("Hotkey F10: shader override effect {}", enabled ? "ON" : "OFF");
        }
        f10WasDown = f10Down;
    }
```

`uIeeEnabled` is fed `g_overridesEnabled ? 1.0f : 0.0f` in the `glUseProgram` uniform feed (Task 6 Step 2.5). Replacement shaders branch on it — instant A/B without recompiling.

- [ ] **Step 2: Commit + CI push** — `git commit -m "probe: add F10 A/B hotkey for override effect"`.

---

### Task 10: Gate runbook V1/V2/V5 (in-game session)

**Files:**
- Create: `docs/validation/phase0-gates.md`

This task is a **manual in-game session** on the Windows machine using the Task 1-9 build, with `EnableOverrides = false`, `DumpEngineShaders = true`, verbose logging on.

- [ ] **Step 1: Create the runbook doc** with this exact structure, then fill it during the session:

```markdown
# Phase 0 Gate Evidence

Build: <git sha>   Date: <date>   Game: BGEE 2.6.6.x

## V2 — do our hooks see shader compilation?
Procedure: clean boot to main menu, load a save, check log for
"GL shader source" capture lines from detour_glShaderSource.
- Captures on boot: YES/NO (paste first capture line or note absence)
- If NO: toggle fullscreen / change resolution in options; recompile captured? YES/NO
- Retroactive dumps present in iee-shader-dumps/ (works regardless): list files
Verdict: PASS (compile interception works [at boot | after device reset]) / FAIL
Consequence if reset-only: fpSELECT task must include the reset trigger step.

## V1 — DrawColorTone <-> glUseProgram correlation
Procedure: with verbose logging, load an area, scroll. Add temporary
LOG_DEBUG_FAST("DrawColorTone({})", mode) in Detour_DrawColorTone if not present.
Paste 20 consecutive log lines showing DrawColorTone(N) followed by
"GL program use: program=P".
- Mapping observed: tone 8 -> program <P8>? tone 1 -> <P1>?
- Does glUseProgram fire per DrawColorTone call, or batched?
Verdict: program selector hypothesis CONFIRMED / REJECTED (+ implications)

## V5 — engine FBO usage
Procedure: full session (menu, area, movie if possible, save/load).
- "V5: engine bound FBO" warnings: NONE / list
Verdict: PASS (no engine FBO use) / document usage found

## Slot table re-verification
From glUseProgram introspection logs, paste the slot inference lines
(program=X slot=Y vertex=vpZ fragment=fpW) and diff against spec §1 table.
```

- [ ] **Step 2: Run the session, fill the doc, commit** — `git commit -m "docs: record phase 0 gate evidence (V1, V2, V5)"`. **STOP and review with Gabriel if V2 is a hard FAIL (no interception even after device reset)** — that reshapes Phase 1.

---

### Task 11: Phase 1 — fpSELECT replacement

**Files:**
- Create: `assets/shaders/fpSELECT.glsl`
- Modify: `CMakeLists.txt` (bundle `assets/shaders/` into release bundle as `iee-shaders/`)
- Modify: `tools/InfinityEngine-Enhancer.sample.ini` (document `[Shaders]` keys)

- [ ] **Step 1: Read the dumped original.** From Task 10's session, `iee-shader-dumps/fpSELECT.glsl` exists (retroactive archival). Record its full interface: every `uniform`, `varying`, and `attribute` declaration. The replacement below assumes the common engine pattern (`varying vec2 vTc;` + a sampler + color via `gl_Color` path); **adapt identifier names to match the dump exactly** — the Task 5 contract check will refuse mismatches at runtime, but fix them at authoring time.

- [ ] **Step 2: Write `assets/shaders/fpSELECT.glsl`** — same visual output as the engine, plus screen-space anti-aliasing on the alpha edge, gated by `uIeeEnabled`:

```glsl
// fpSELECT.glsl  (IEE replacement: AA on selection ring edges)
#version 460 compatibility

// ---- engine interface: keep names identical to the dumped original ----
uniform sampler2D sTex;      // adapt name to dump
varying vec2 vTc;            // adapt name to dump
// (re-declare every uniform/varying from the dump, even if unused here)

// ---- IEE additions ----
uniform float uIeeTime;      // seconds, fed per frame (unused here, reserved)
uniform float uIeeEnabled;   // 0/1 hotkey A/B

void main() {
    vec4 texel = texture2D(sTex, vTc);
    vec4 engineColor = texel * gl_Color;        // adapt to mirror the dumped main() exactly

    if (uIeeEnabled > 0.5) {
        // Anti-alias the alpha edge: smooth over one screen-pixel footprint.
        float a = engineColor.a;
        float w = fwidth(a);
        engineColor.a = smoothstep(0.5 - w, 0.5 + w, a);
    }

    gl_FragColor = engineColor;
}
```

The mandatory authoring rule: the part above the IEE block must reproduce the dumped original's math 1:1 (so `uIeeEnabled = 0` is pixel-identical), and every dumped identifier must appear.

- [ ] **Step 3: Bundle wiring** in `CMakeLists.txt` `release_bundle` target, add:

```cmake
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets/shaders" "${IEE_RELEASE_BUNDLE_DIR}/iee-shaders"
```

and document in `tools/InfinityEngine-Enhancer.sample.ini`:

```ini
[Shaders]
EnableOverrides = false
DumpEngineShaders = true
OverrideDir = iee-shaders
MagentaShaders =
EnableDebugHotkeys = false
```

- [ ] **Step 4: In-game proof session.** Install bundle with `EnableOverrides = true`, `EnableDebugHotkeys = true`, copy `iee-shaders/` next to the DLL. Trigger a shader recompile per V2's verdict (boot-time interception, or fullscreen/resolution toggle). Then in-game: select a character, F10-toggle, observe ring edges. Evidence: log shows `Shader override applied: fpSELECT`, no `falling back` warning; screenshots ON/OFF. Record in `docs/validation/phase0-gates.md` under a new `## Phase 1 proof` heading.

- [ ] **Step 5: Commit** — `git commit -m "shaders: add fpSELECT replacement with edge AA (pipeline proof)"`.

---

### Task 12: Gate V3 + conditional fpCatRom replacement

**Files:**
- Create: `assets/shaders/fpCatRom.glsl` (only if V3 confirms it matters)
- Modify: `docs/validation/phase0-gates.md`

- [ ] **Step 1: V3 magenta session.** Set `MagentaShaders = fpCatRom` in the INI, run the game, note exactly what turns magenta (whole window during play? only movies? only the zoomed map?). Record verdict in the gates doc: `V3: fpCatRom covers <X>`.

- [ ] **Step 2 (only if V3 shows it covers gameplay-relevant blits): write `assets/shaders/fpCatRom.glsl`.** Same interface-mirroring rule as Task 11 (dump is in `iee-shader-dumps/fpCatRom.glsl`). Replacement body — proper Catmull-Rom using 4 bilinear fetches (Mitchell trick), `textureSize()` avoids needing extra engine uniforms:

```glsl
// fpCatRom.glsl (IEE replacement: correct Catmull-Rom resampling)
#version 460 compatibility

uniform sampler2D sTex;     // adapt names to dump
varying vec2 vTc;
uniform float uIeeEnabled;

vec4 catmullRom(sampler2D tex, vec2 uv) {
    vec2 texSize = vec2(textureSize(tex, 0));
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 texPos0 = (texPos1 - 1.0) / texSize;
    vec2 texPos3 = (texPos1 + 2.0) / texSize;
    vec2 texPos12 = (texPos1 + offset12) / texSize;

    vec4 result =
        texture2D(tex, vec2(texPos0.x,  texPos0.y))  * w0.x  * w0.y +
        texture2D(tex, vec2(texPos12.x, texPos0.y))  * w12.x * w0.y +
        texture2D(tex, vec2(texPos3.x,  texPos0.y))  * w3.x  * w0.y +
        texture2D(tex, vec2(texPos0.x,  texPos12.y)) * w0.x  * w12.y +
        texture2D(tex, vec2(texPos12.x, texPos12.y)) * w12.x * w12.y +
        texture2D(tex, vec2(texPos3.x,  texPos12.y)) * w3.x  * w12.y +
        texture2D(tex, vec2(texPos0.x,  texPos3.y))  * w0.x  * w3.y +
        texture2D(tex, vec2(texPos12.x, texPos3.y))  * w12.x * w3.y +
        texture2D(tex, vec2(texPos3.x,  texPos3.y))  * w3.x  * w3.y;
    return result;
}

void main() {
    if (uIeeEnabled > 0.5) {
        gl_FragColor = catmullRom(sTex, vTc);
    } else {
        gl_FragColor = /* mirror the dumped original's main() exactly */ texture2D(sTex, vTc);
    }
}
```

- [ ] **Step 3: In-game A/B** (F10), screenshots at the blit's coverage area, record in gates doc.

- [ ] **Step 4: Commit** — `git commit -m "shaders: add fpCatRom bicubic replacement"` (or, if V3 says skip: commit only the gates-doc verdict).

---

### Task 13: Wrap-up

- [ ] **Step 1: Docs touch-up** — update `docs/architecture.md` Runtime Modules with the new modules (`shader_probe`, `frame_hook`, `area_state`, `features/tile_render`, `game/shader_override`, `game/area_texture`), one line each, matching the existing style.
- [ ] **Step 2: Full local tests + CI green** — `ctest --test-dir cmake-build-debug --output-on-failure` then push, watch run.
- [ ] **Step 3: Use superpowers:finishing-a-development-branch** to decide merge/PR.

---

## Self-Review Notes (done at plan time)

- **Spec coverage:** Phase 0 bullets — probe port (T6), state guard (T6), area texture + uniform bridge (T6/T8), SDL hook (T8), hotkeys (T9), prologue sanity check — **deliberately deferred**: MinHook's `MH_CreateHook` already fails loudly on unhookable prologues, which covers the EEex-collision insurance for Phase 0/1's GL-pointer targets (engine-function hooks in later phases should add it). Gates V1/V2/V5 (T10), V3 (T12); refactors (T3/T7); host tests (T2/T5/T8). Phase 1 — fpSELECT (T11), fpCatRom (T12).
- **Known judgment calls:** probe stays lazily installed from first render (WIP-proven; wglGetProcAddress needs a context); V2's runbook explicitly measures the consequence. Area-texture upload thread-affinity risk is flagged inline with its fallback (move to frame tick).
- **Types:** `TileRenderState` names match between T7 header and usage; `pack_area_cell_texture` consistent T8 test/impl; probe globals (`g_overriddenPrograms`, `g_overridesEnabled`, `g_uniformTime`) consistent T6/T9.
