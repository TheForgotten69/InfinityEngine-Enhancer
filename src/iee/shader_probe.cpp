#include "shader_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <intrin.h>
#include <windows.h>
#include <dbghelp.h>

#include <spdlog/fmt/fmt.h>

#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"
#include "iee/game/opengl_types.h"
#include "iee/game/shader_override.h"

namespace iee::probe {
    namespace {
        // region GL constants
        constexpr unsigned SHADER_TYPE            = 0x8B4F;
        constexpr unsigned ATTACHED_SHADERS       = 0x8B85;
        constexpr unsigned COMPILE_STATUS         = 0x8B81;
        constexpr unsigned LINK_STATUS            = 0x8B82;
        constexpr unsigned INFO_LOG_LENGTH        = 0x8B84;
        constexpr unsigned VERTEX_SHADER          = 0x8B31;
        constexpr unsigned FRAGMENT_SHADER        = 0x8B30;
        constexpr unsigned SHADER_SOURCE_LENGTH   = 0x8B88;
        // endregion

        // region Data records
        struct ShaderRecord {
            std::string sourcePreview;
            std::size_t sourceBytes{};
            bool sawShaderSource{};
            bool compileLogged{};
            std::string shaderName;       // added: name extracted at glShaderSource time
            bool overrideApplied{};       // added: true when substituted source was submitted
        };

        struct ProgramRecord {
            bool linkLogged{};
            bool useLogged{};
            std::unordered_map<std::uintptr_t, bool> callerLogged;
            std::optional<int> inferredSlot;
            std::string vertexShaderName;
            std::string fragmentShaderName;
        };

        struct UniformLocations {
            int time{-2};    // -2 = not yet queried; -1 = queried, not found
            int enabled{-2};
        };
        // endregion

        // region Caller info (dbghelp)
        struct CallerInfo {
            std::uintptr_t caller{};
            std::optional<std::uintptr_t> callerRva;
            std::string moduleName;
            std::string modulePath;
            std::string symbolName;
            std::uint64_t symbolDisplacement{};
        };
        // endregion

        // region Function pointer typedefs (must match opengl_types.h exactly)
        using Fn_glShaderSource        = void (APIENTRY*)(unsigned, int, const char* const*, const int*);
        using Fn_glCompileShader       = void (APIENTRY*)(unsigned);
        using Fn_glLinkProgram         = void (APIENTRY*)(unsigned);
        using Fn_glUseProgram          = void (APIENTRY*)(unsigned);
        using Fn_glShaderSourceARB     = void (APIENTRY*)(unsigned, int, const char* const*, const int*);
        using Fn_glCompileShaderARB    = void (APIENTRY*)(unsigned);
        using Fn_glLinkProgramARB      = void (APIENTRY*)(unsigned);
        using Fn_glUseProgramObjectARB = void (APIENTRY*)(unsigned);
        using Fn_glBindFramebuffer     = void (APIENTRY*)(unsigned, unsigned);
        // endregion

        // region Hook objects
        core::Hook<Fn_glShaderSource>        g_glShaderSourceHook;
        core::Hook<Fn_glCompileShader>       g_glCompileShaderHook;
        core::Hook<Fn_glLinkProgram>         g_glLinkProgramHook;
        core::Hook<Fn_glUseProgram>          g_glUseProgramHook;
        core::Hook<Fn_glShaderSourceARB>     g_glShaderSourceARBHook;
        core::Hook<Fn_glCompileShaderARB>    g_glCompileShaderARBHook;
        core::Hook<Fn_glLinkProgramARB>      g_glLinkProgramARBHook;
        core::Hook<Fn_glUseProgramObjectARB> g_glUseProgramObjectARBHook;
        core::Hook<Fn_glBindFramebuffer>     g_glBindFramebufferHook;
        // endregion

        // region Module-level state
        std::mutex g_probeMutex;
        std::unordered_map<unsigned, ShaderRecord>   g_shaderRecords;
        std::unordered_map<unsigned, ProgramRecord>  g_programRecords;
        std::unordered_map<unsigned, UniformLocations> g_overriddenPrograms; // programs with at least one overridden shader
        std::unordered_map<unsigned, std::string>    g_pendingFallback;      // shader -> original source (pre-override)
        std::set<std::string, std::less<>>           g_magentaShaders;       // upper-cased names
        std::set<std::string>                        g_dumpedShaders;        // names already written to disk
        bool                                         g_shaderProbesInstalled = false;
        core::EngineConfig                           g_cfg;
        game::ShaderOverrideRegistry                 g_registry;
        std::filesystem::path                        g_dumpDir;

        std::atomic<float> g_uniformTime{0.0f};
        std::atomic<bool>  g_overridesEnabled{true};
        // endregion

        // region String helpers
        std::string sanitize_preview(std::string text) {
            for (char &ch : text) {
                if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
            }
            constexpr std::size_t kMaxPreviewBytes = 240;
            if (text.size() > kMaxPreviewBytes) {
                text.resize(kMaxPreviewBytes);
                text += "...";
            }
            return text;
        }

        std::string upper_copy(std::string_view value) {
            std::string out(value);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            return out;
        }

        void configure_magenta_shaders(std::string_view list) {
            g_magentaShaders.clear();
            std::string current;
            for (const char ch : list) {
                if (ch == ',') {
                    if (!current.empty()) {
                        g_magentaShaders.insert(upper_copy(current));
                        current.clear();
                    }
                    continue;
                }
                if (!std::isspace(static_cast<unsigned char>(ch))) {
                    current.push_back(ch);
                }
            }
            if (!current.empty()) {
                g_magentaShaders.insert(upper_copy(current));
            }
        }

        // Joins ALL chunks into a single string, honouring lengths[i] >= 0.
        std::string gather_full_source(int count, const char* const* strings, const int* lengths) {
            if (count <= 0 || !strings) return {};
            std::string combined;
            for (int i = 0; i < count; ++i) {
                const char *chunk = strings[i];
                if (!chunk) continue;
                std::size_t chunkSize = (lengths && lengths[i] >= 0)
                    ? static_cast<std::size_t>(lengths[i])
                    : std::char_traits<char>::length(chunk);
                combined.append(chunk, chunkSize);
            }
            return combined;
        }

        std::string sanitized_preview_of(std::string_view source) {
            std::string s(source.substr(0, 256));
            return sanitize_preview(std::move(s));
        }
        // endregion

        // region GL query helpers
        const char *shader_type_name(int shaderType) noexcept {
            switch (shaderType) {
                case VERTEX_SHADER:   return "vertex";
                case FRAGMENT_SHADER: return "fragment";
                default:              return "unknown";
            }
        }

        std::string read_shader_log(const game::gl::OpenGLFunctions &gl, unsigned shader) {
            if (!gl.glGetShaderiv || !gl.glGetShaderInfoLog) return {};
            int infoLogLength = 0;
            gl.glGetShaderiv(shader, INFO_LOG_LENGTH, &infoLogLength);
            if (infoLogLength <= 1) return {};
            std::string infoLog(static_cast<std::size_t>(infoLogLength), '\0');
            int written = 0;
            gl.glGetShaderInfoLog(shader, infoLogLength, &written, infoLog.data());
            if (written > 0 && written < infoLogLength) infoLog.resize(static_cast<std::size_t>(written));
            return sanitize_preview(std::move(infoLog));
        }

        std::string read_program_log(const game::gl::OpenGLFunctions &gl, unsigned program) {
            if (!gl.glGetProgramiv || !gl.glGetProgramInfoLog) return {};
            int infoLogLength = 0;
            gl.glGetProgramiv(program, INFO_LOG_LENGTH, &infoLogLength);
            if (infoLogLength <= 1) return {};
            std::string infoLog(static_cast<std::size_t>(infoLogLength), '\0');
            int written = 0;
            gl.glGetProgramInfoLog(program, infoLogLength, &written, infoLog.data());
            if (written > 0 && written < infoLogLength) infoLog.resize(static_cast<std::size_t>(written));
            return sanitize_preview(std::move(infoLog));
        }

        std::string read_shader_source_preview(const game::gl::OpenGLFunctions &gl, unsigned shader) {
            if (!gl.glGetShaderSource) return {};
            constexpr int kMaxSourceBytes = 1024;
            std::array<char, kMaxSourceBytes> buffer{};
            int written = 0;
            gl.glGetShaderSource(shader, kMaxSourceBytes, &written, buffer.data());
            if (written <= 0) return {};
            return sanitize_preview(std::string(buffer.data(), static_cast<std::size_t>(written)));
        }

        std::string_view get_gl_string(const game::gl::OpenGLFunctions &gl, unsigned name) noexcept {
            if (!gl.glGetString) return {};
            const auto *value = gl.glGetString(name);
            if (!value) return {};
            return reinterpret_cast<const char *>(value);
        }

        std::string basename(std::string path) {
            const auto pos = path.find_last_of("\\/");
            if (pos == std::string::npos) return path;
            return path.substr(pos + 1);
        }
        // endregion

        // region Caller info (dbghelp)
        CallerInfo current_caller_info() noexcept {
            CallerInfo info{};
            info.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            const auto module = core::get_module_span(nullptr);
            if (module) {
                const auto moduleBase = reinterpret_cast<std::uintptr_t>(module->base);
                const auto moduleEnd  = moduleBase + module->size;
                if (info.caller >= moduleBase && info.caller < moduleEnd) {
                    info.callerRva = info.caller - moduleBase;
                }
            }

            HMODULE callerModule = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(info.caller),
                                   &callerModule)) {
                std::array<char, MAX_PATH> pathBuffer{};
                const auto written = GetModuleFileNameA(callerModule,
                                                        pathBuffer.data(),
                                                        static_cast<DWORD>(pathBuffer.size()));
                if (written > 0) {
                    info.modulePath.assign(pathBuffer.data(), written);
                    info.moduleName = basename(info.modulePath);
                }
            }

            static std::once_flag symInitOnce;
            static bool symbolsReady = false;
            std::call_once(symInitOnce, []() {
                symbolsReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) == TRUE;
            });

            if (symbolsReady) {
                std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
                auto *symbol = reinterpret_cast<SYMBOL_INFO *>(symbolBuffer.data());
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = MAX_SYM_NAME;
                DWORD64 displacement = 0;
                if (SymFromAddr(GetCurrentProcess(),
                                static_cast<DWORD64>(info.caller),
                                &displacement, symbol)) {
                    info.symbolName.assign(symbol->Name, symbol->NameLen);
                    info.symbolDisplacement = displacement;
                }
            }
            return info;
        }

        std::string caller_summary(const CallerInfo &callerInfo) {
            std::string summary = fmt::format("caller=0x{:X}", callerInfo.caller);
            if (callerInfo.callerRva) {
                summary += fmt::format(" rva=0x{:X}", *callerInfo.callerRva);
            }
            if (!callerInfo.moduleName.empty()) {
                summary += fmt::format(" module={}", callerInfo.moduleName);
            }
            if (!callerInfo.symbolName.empty()) {
                if (callerInfo.symbolDisplacement == 0) {
                    summary += fmt::format(" symbol={}", callerInfo.symbolName);
                } else {
                    summary += fmt::format(" symbol={}+0x{:X}", callerInfo.symbolName, callerInfo.symbolDisplacement);
                }
            }
            return summary;
        }
        // endregion

        // region Slot inference
        std::optional<int> infer_program_slot(std::string_view vertexShaderName,
                                              std::string_view fragmentShaderName) {
            if (vertexShaderName == "vpDraw"  && fragmentShaderName == "fpDraw")    return 0;
            if (vertexShaderName == "vpDraw"  && fragmentShaderName == "fpTone")    return 1;
            if (vertexShaderName == "vpBlit"  && fragmentShaderName == "fpCatRom")  return 2;
            if ((vertexShaderName == "vpYUV"  || vertexShaderName == "vpDraw") &&
                fragmentShaderName == "fpYUV")                                       return 3;
            if (vertexShaderName == "vpYUV"   && fragmentShaderName == "fpYUVGRY") return 4;
            if (vertexShaderName == "vpDraw"  && fragmentShaderName == "fpSprite")  return 5;
            if (vertexShaderName == "vpDraw"  && fragmentShaderName == "fpFONT")    return 6;
            if (vertexShaderName == "vpDraw"  && fragmentShaderName == "fpSELECT")  return 7;
            if (vertexShaderName == "vpDraw"  && fragmentShaderName == "fpSEAM")    return 8;
            return std::nullopt;
        }
        // endregion

        // region Dump helper
        void dump_shader_to_disk(const std::string &name, const std::string &fullSource) noexcept {
            if (g_dumpDir.empty()) return;
            std::error_code ec;
            std::filesystem::create_directories(g_dumpDir, ec);
            if (ec) {
                LOG_WARN("shader dump: could not create directory {}: {}", g_dumpDir.string(), ec.message());
                return;
            }
            const auto filePath = g_dumpDir / (name + ".glsl");
            std::ofstream f(filePath, std::ios::trunc | std::ios::binary);
            if (!f) {
                LOG_WARN("shader dump: could not open {} for writing", filePath.string());
                return;
            }
            f.write(fullSource.data(), static_cast<std::streamsize>(fullSource.size()));
            LOG_INFO("shader dump: wrote {} ({} bytes)", filePath.string(), fullSource.size());
        }
        // endregion

        // region Detour: glShaderSource (CHANGE 3)
        static void APIENTRY detour_glShaderSource(unsigned shader, int count,
                                                   const char* const* string, const int* length) {
            const std::string fullSource = gather_full_source(count, string, length);
            const auto name = game::extract_shader_name(fullSource, "");
            std::string substituted;

            if (!name.empty()) {
                if (g_magentaShaders.contains(upper_copy(name))) {
                    substituted = game::make_magenta_variant(fullSource);
                    if (!substituted.empty()) {
                        LOG_INFO("Applied magenta debug patch to shader {}", name);
                    }
                }
                if (substituted.empty() && g_cfg.enableShaderOverrides) {
                    if (const auto replacement = g_registry.find(name)) {
                        const auto contract = game::check_interface_contract(fullSource, *replacement);
                        if (contract.ok) {
                            substituted = std::string(*replacement);
                            LOG_INFO("Shader override applied: {} ({} bytes)", name, substituted.size());
                        } else {
                            for (const auto &id : contract.missingIdentifiers) {
                                LOG_WARN("Override {} missing interface identifier '{}' - falling back to engine source",
                                         name, id);
                            }
                        }
                    }
                }
            }

            {
                std::lock_guard lock(g_probeMutex);
                auto &record = g_shaderRecords[shader];
                const std::string &effective = substituted.empty() ? fullSource : substituted;
                record.sourcePreview = sanitized_preview_of(effective);
                record.sourceBytes   = effective.size();
                record.sawShaderSource = true;
                record.compileLogged   = false;
                record.shaderName      = name;
                record.overrideApplied = !substituted.empty() && !name.empty();
                if (record.overrideApplied) {
                    g_pendingFallback[shader] = fullSource;
                } else {
                    g_pendingFallback.erase(shader);
                }
            }

            if (!substituted.empty()) {
                const char *ptr = substituted.c_str();
                const int   len = static_cast<int>(substituted.size());
                g_glShaderSourceHook.original()(shader, 1, &ptr, &len);
            } else {
                g_glShaderSourceHook.original()(shader, count, string, length);
            }
        }

        static void APIENTRY detour_glShaderSourceARB(unsigned shader, int count,
                                                      const char* const* string, const int* length) {
            const std::string fullSource = gather_full_source(count, string, length);
            const auto name = game::extract_shader_name(fullSource, "");
            std::string substituted;

            if (!name.empty()) {
                if (g_magentaShaders.contains(upper_copy(name))) {
                    substituted = game::make_magenta_variant(fullSource);
                    if (!substituted.empty()) {
                        LOG_INFO("Applied magenta debug patch to shader {} (ARB)", name);
                    }
                }
                if (substituted.empty() && g_cfg.enableShaderOverrides) {
                    if (const auto replacement = g_registry.find(name)) {
                        const auto contract = game::check_interface_contract(fullSource, *replacement);
                        if (contract.ok) {
                            substituted = std::string(*replacement);
                            LOG_INFO("Shader override applied (ARB): {} ({} bytes)", name, substituted.size());
                        } else {
                            for (const auto &id : contract.missingIdentifiers) {
                                LOG_WARN("Override {} (ARB) missing interface identifier '{}' - falling back to engine source",
                                         name, id);
                            }
                        }
                    }
                }
            }

            {
                std::lock_guard lock(g_probeMutex);
                auto &record = g_shaderRecords[shader];
                const std::string &effective = substituted.empty() ? fullSource : substituted;
                record.sourcePreview = sanitized_preview_of(effective);
                record.sourceBytes   = effective.size();
                record.sawShaderSource = true;
                record.compileLogged   = false;
                record.shaderName      = name;
                record.overrideApplied = !substituted.empty() && !name.empty();
                if (record.overrideApplied) {
                    g_pendingFallback[shader] = fullSource;
                } else {
                    g_pendingFallback.erase(shader);
                }
            }

            if (!substituted.empty()) {
                const char *ptr = substituted.c_str();
                const int   len = static_cast<int>(substituted.size());
                g_glShaderSourceARBHook.original()(shader, 1, &ptr, &len);
            } else {
                g_glShaderSourceARBHook.original()(shader, count, string, length);
            }
        }
        // endregion

        // region Detour: glCompileShader (CHANGE 4)
        static void APIENTRY detour_glCompileShader(unsigned shader) {
            g_glCompileShaderHook.original()(shader);

            const auto &gl = game::gl::get_gl_functions();
            int shaderType    = 0;
            int compileStatus = 0;
            if (gl.glGetShaderiv) {
                gl.glGetShaderiv(shader, SHADER_TYPE,     &shaderType);
                gl.glGetShaderiv(shader, COMPILE_STATUS,  &compileStatus);
            }

            // Compile-failure fallback: revert to original source if override failed
            if (compileStatus == 0) {
                std::string fallbackSource;
                std::string shaderName;
                {
                    std::lock_guard lock(g_probeMutex);
                    auto it = g_pendingFallback.find(shader);
                    if (it != g_pendingFallback.end()) {
                        fallbackSource = it->second;
                        auto &record = g_shaderRecords[shader];
                        shaderName = record.shaderName;
                    }
                }
                if (!fallbackSource.empty()) {
                    const char *ptr = fallbackSource.c_str();
                    const int   len = static_cast<int>(fallbackSource.size());
                    // Call ORIGINAL functions — no recursion
                    g_glShaderSourceHook.original()(shader, 1, &ptr, &len);
                    g_glCompileShaderHook.original()(shader);

                    // Read info log from the reverted compile
                    const auto infoLog = read_shader_log(gl, shader);
                    LOG_ERROR("Override for shader {} failed to compile; reverted to engine source (log: {})",
                              shaderName, infoLog);

                    std::lock_guard lock(g_probeMutex);
                    g_pendingFallback.erase(shader);
                    auto &record = g_shaderRecords[shader];
                    record.overrideApplied = false;
                    record.compileLogged   = true;
                    return;
                }
            } else {
                // Compile succeeded — clear any pending fallback
                std::lock_guard lock(g_probeMutex);
                g_pendingFallback.erase(shader);
            }

            std::string preview;
            std::size_t sourceBytes = 0;
            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto &record = g_shaderRecords[shader];
                shouldLog = !record.compileLogged;
                record.compileLogged = true;
                preview     = record.sourcePreview;
                sourceBytes = record.sourceBytes;
            }

            if (!shouldLog) return;

            const auto infoLog = read_shader_log(gl, shader);
            LOG_INFO("GL shader compile: shader={} type={} status={} bytes={} preview={}",
                     shader,
                     shader_type_name(shaderType),
                     compileStatus != 0 ? "ok" : "fail",
                     sourceBytes,
                     preview.empty() ? "<empty>" : preview);
            if (!infoLog.empty()) {
                LOG_INFO("GL shader compile log: shader={} log={}", shader, infoLog);
            }
        }

        static void APIENTRY detour_glCompileShaderARB(unsigned shader) {
            g_glCompileShaderARBHook.original()(shader);

            const auto &gl = game::gl::get_gl_functions();
            int shaderType    = 0;
            int compileStatus = 0;
            if (gl.glGetShaderiv) {
                gl.glGetShaderiv(shader, SHADER_TYPE,     &shaderType);
                gl.glGetShaderiv(shader, COMPILE_STATUS,  &compileStatus);
            }

            if (compileStatus == 0) {
                std::string fallbackSource;
                std::string shaderName;
                {
                    std::lock_guard lock(g_probeMutex);
                    auto it = g_pendingFallback.find(shader);
                    if (it != g_pendingFallback.end()) {
                        fallbackSource = it->second;
                        auto &record = g_shaderRecords[shader];
                        shaderName = record.shaderName;
                    }
                }
                if (!fallbackSource.empty()) {
                    const char *ptr = fallbackSource.c_str();
                    const int   len = static_cast<int>(fallbackSource.size());
                    g_glShaderSourceARBHook.original()(shader, 1, &ptr, &len);
                    g_glCompileShaderARBHook.original()(shader);

                    const auto infoLog = read_shader_log(gl, shader);
                    LOG_ERROR("Override for shader {} (ARB) failed to compile; reverted to engine source (log: {})",
                              shaderName, infoLog);

                    std::lock_guard lock(g_probeMutex);
                    g_pendingFallback.erase(shader);
                    auto &record = g_shaderRecords[shader];
                    record.overrideApplied = false;
                    record.compileLogged   = true;
                    return;
                }
            } else {
                std::lock_guard lock(g_probeMutex);
                g_pendingFallback.erase(shader);
            }

            std::string preview;
            std::size_t sourceBytes = 0;
            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto &record = g_shaderRecords[shader];
                shouldLog = !record.compileLogged;
                record.compileLogged = true;
                preview     = record.sourcePreview;
                sourceBytes = record.sourceBytes;
            }

            if (!shouldLog) return;

            const auto infoLog = read_shader_log(gl, shader);
            LOG_INFO("GL shader compile (ARB): shader={} type={} status={} bytes={} preview={}",
                     shader,
                     shader_type_name(shaderType),
                     compileStatus != 0 ? "ok" : "fail",
                     sourceBytes,
                     preview.empty() ? "<empty>" : preview);
            if (!infoLog.empty()) {
                LOG_INFO("GL shader compile log (ARB): shader={} log={}", shader, infoLog);
            }
        }
        // endregion

        // region Helper: retroactive shader dump (called from both use-program and link paths)
        // Checks g_dumpedShaders under the lock, then releases before all GL/file work,
        // then re-locks to insert the record — never holds g_probeMutex across GL calls.
        void maybe_dump_engine_shader(const game::gl::OpenGLFunctions &gl, unsigned shader,
                                      const std::string &name) {
            if (!g_cfg.dumpEngineShaders) return;
            if (name.empty()) return;

            // Check under lock whether already done or override applied
            bool alreadyDumped = false;
            bool overrideApplied = false;
            {
                std::lock_guard lock(g_probeMutex);
                alreadyDumped = g_dumpedShaders.contains(name);
                if (!alreadyDumped) {
                    auto it = g_shaderRecords.find(shader);
                    if (it != g_shaderRecords.end()) {
                        overrideApplied = it->second.overrideApplied;
                    }
                }
            }
            if (alreadyDumped) return;

            if (overrideApplied) {
                LOG_INFO("shader dump: skipping {} (override applied — would archive our own source)", name);
                std::lock_guard lock(g_probeMutex);
                g_dumpedShaders.insert(name);
                return;
            }

            // GL work outside the lock
            if (gl.glGetShaderiv && gl.glGetShaderSource) {
                int srcLen = 0;
                gl.glGetShaderiv(shader, SHADER_SOURCE_LENGTH, &srcLen);
                if (srcLen > 1) {
                    std::string fullSrc(static_cast<std::size_t>(srcLen), '\0');
                    int written = 0;
                    gl.glGetShaderSource(shader, srcLen, &written, fullSrc.data());
                    if (written > 0) {
                        fullSrc.resize(static_cast<std::size_t>(written));
                        dump_shader_to_disk(name, fullSrc);
                    }
                }
            }

            std::lock_guard lock(g_probeMutex);
            g_dumpedShaders.insert(name);
        }
        // endregion

        // region Helper: link program introspection (shader walk + archival + tracking)
        void link_program_introspect(unsigned program, bool isArb) {
            const auto &gl = game::gl::get_gl_functions();
            if (!gl.glGetProgramiv || !gl.glGetAttachedShaders || !gl.glGetShaderiv) return;

            int attachedShaderCount = 0;
            gl.glGetProgramiv(program, ATTACHED_SHADERS, &attachedShaderCount);
            if (attachedShaderCount <= 0) return;

            std::vector<unsigned> shaders(static_cast<std::size_t>(attachedShaderCount), 0u);
            int actualShaderCount = 0;
            gl.glGetAttachedShaders(program, attachedShaderCount, &actualShaderCount, shaders.data());

            std::string vertexShaderName;
            std::string fragmentShaderName;
            bool anyOverride = false;

            for (int i = 0; i < actualShaderCount; ++i) {
                const unsigned s = shaders[static_cast<std::size_t>(i)];
                int shaderType = 0;
                gl.glGetShaderiv(s, SHADER_TYPE, &shaderType);

                // Determine name: prefer cached record, fall back to glGetShaderSource query
                std::string nameForShader;
                bool overrideAppliedForShader = false;
                {
                    std::lock_guard lock(g_probeMutex);
                    auto it = g_shaderRecords.find(s);
                    if (it != g_shaderRecords.end()) {
                        nameForShader = it->second.shaderName;
                        overrideAppliedForShader = it->second.overrideApplied;
                    }
                }

                // Archival: fetch full source and write to disk (engine source only, not our override)
                maybe_dump_engine_shader(gl, s, nameForShader);

                const auto preview = read_shader_source_preview(gl, s);
                // If we don't have a name from the record, try reading from the GL source preview
                if (nameForShader.empty()) {
                    nameForShader = game::extract_shader_name(preview,
                        shaderType == VERTEX_SHADER ? "vp" : "fp");
                }

                if (shaderType == VERTEX_SHADER)        vertexShaderName   = nameForShader;
                else if (shaderType == FRAGMENT_SHADER) fragmentShaderName = nameForShader;

                if (overrideAppliedForShader) anyOverride = true;

                LOG_INFO("GL program attached shader{}: program={} shader={} type={} preview={}",
                         isArb ? " (ARB)" : "",
                         program, s,
                         shader_type_name(shaderType),
                         preview);
            }

            const auto inferredSlot = infer_program_slot(vertexShaderName, fragmentShaderName);
            {
                std::lock_guard lock(g_probeMutex);
                auto &record = g_programRecords[program];
                record.inferredSlot      = inferredSlot;
                record.vertexShaderName  = vertexShaderName;
                record.fragmentShaderName = fragmentShaderName;

                // Program tracking for uniform feed
                if (anyOverride) {
                    g_overriddenPrograms[program] = UniformLocations{-2, -2};
                }
            }

            if (inferredSlot) {
                LOG_INFO("GL program slot inference{}: program={} slot={} vertex={} fragment={}",
                         isArb ? " (ARB)" : "",
                         program, *inferredSlot, vertexShaderName, fragmentShaderName);
            } else {
                LOG_INFO("GL program slot inference{}: program={} slot=<unknown> vertex={} fragment={}",
                         isArb ? " (ARB)" : "",
                         program,
                         vertexShaderName.empty()   ? "<unknown>" : vertexShaderName,
                         fragmentShaderName.empty() ? "<unknown>" : fragmentShaderName);
            }
        }
        // endregion

        // region Detour: glLinkProgram (CHANGE 5 — program tracking)
        static void APIENTRY detour_glLinkProgram(unsigned program) {
            g_glLinkProgramHook.original()(program);

            const auto &gl = game::gl::get_gl_functions();
            int linkStatus = 0;
            if (gl.glGetProgramiv) {
                gl.glGetProgramiv(program, LINK_STATUS, &linkStatus);
            }

            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto &record = g_programRecords[program];
                shouldLog = !record.linkLogged;
                record.linkLogged = true;
                // Reset uniform locations on relink
                if (g_overriddenPrograms.contains(program)) {
                    g_overriddenPrograms[program] = UniformLocations{-2, -2};
                }
            }

            if (!shouldLog) return;

            const auto infoLog = read_program_log(gl, program);
            LOG_INFO("GL program link: program={} status={}",
                     program, linkStatus != 0 ? "ok" : "fail");
            if (!infoLog.empty()) {
                LOG_INFO("GL program link log: program={} log={}", program, infoLog);
            }

            link_program_introspect(program, false);
        }

        static void APIENTRY detour_glLinkProgramARB(unsigned program) {
            g_glLinkProgramARBHook.original()(program);

            const auto &gl = game::gl::get_gl_functions();
            int linkStatus = 0;
            if (gl.glGetProgramiv) {
                gl.glGetProgramiv(program, LINK_STATUS, &linkStatus);
            }

            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto &record = g_programRecords[program];
                shouldLog = !record.linkLogged;
                record.linkLogged = true;
                if (g_overriddenPrograms.contains(program)) {
                    g_overriddenPrograms[program] = UniformLocations{-2, -2};
                }
            }

            if (!shouldLog) return;

            const auto infoLog = read_program_log(gl, program);
            LOG_INFO("GL program link (ARB): program={} status={}",
                     program, linkStatus != 0 ? "ok" : "fail");
            if (!infoLog.empty()) {
                LOG_INFO("GL program link log (ARB): program={} log={}", program, infoLog);
            }

            link_program_introspect(program, true);
        }
        // endregion

        // region Uniform feed helper
        void feed_uniforms_to_program(unsigned program) noexcept {
            const auto &gl = game::gl::get_gl_functions();
            if (!gl.glGetUniformLocation || !gl.glUniform1f) return;

            UniformLocations locs{};
            {
                std::lock_guard lock(g_probeMutex);
                auto it = g_overriddenPrograms.find(program);
                if (it == g_overriddenPrograms.end()) return;
                locs = it->second;
            }

            // Lazily resolve locations (queried at most once per program per link)
            if (locs.time == -2) {
                locs.time = gl.glGetUniformLocation(program, "uIeeTime");
            }
            if (locs.enabled == -2) {
                locs.enabled = gl.glGetUniformLocation(program, "uIeeEnabled");
            }

            // Store back resolved locations
            {
                std::lock_guard lock(g_probeMutex);
                auto it = g_overriddenPrograms.find(program);
                if (it != g_overriddenPrograms.end()) {
                    it->second = locs;
                }
            }

            // Feed values — program is currently bound (we are post-original in glUseProgram)
            if (locs.time >= 0) {
                gl.glUniform1f(locs.time, g_uniformTime.load(std::memory_order_relaxed));
            }
            if (locs.enabled >= 0) {
                gl.glUniform1f(locs.enabled,
                               g_overridesEnabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
            }
        }
        // endregion

        // region Detour: glUseProgram (CHANGE 5 — uniform feed)
        static void APIENTRY detour_glUseProgram(unsigned program) {
            const auto callerInfo = current_caller_info();
            g_glUseProgramHook.original()(program);

            // Uniform feed (post-original, program is now bound)
            if (program != 0) {
                bool isOverridden = false;
                {
                    std::lock_guard lock(g_probeMutex);
                    isOverridden = g_overriddenPrograms.contains(program);
                }
                if (isOverridden) {
                    feed_uniforms_to_program(program);
                }
            }

            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto &record = g_programRecords[program];
                shouldLog = !record.callerLogged.contains(callerInfo.caller);
                record.useLogged = true;
                record.callerLogged.emplace(callerInfo.caller, true);
            }

            if (shouldLog) {
                LOG_INFO("GL program bind: program={} {}",
                         program, caller_summary(callerInfo));

                if (program != 0) {
                    const auto &gl = game::gl::get_gl_functions();
                    if (gl.glGetProgramiv && gl.glGetAttachedShaders && gl.glGetShaderiv) {
                        int attachedShaderCount = 0;
                        gl.glGetProgramiv(program, ATTACHED_SHADERS, &attachedShaderCount);
                        if (attachedShaderCount > 0) {
                            std::vector<unsigned> shaders(static_cast<std::size_t>(attachedShaderCount), 0u);
                            int actualShaderCount = 0;
                            gl.glGetAttachedShaders(program, attachedShaderCount,
                                                    &actualShaderCount, shaders.data());
                            std::string vertexShaderName;
                            std::string fragmentShaderName;
                            for (int idx = 0; idx < actualShaderCount; ++idx) {
                                const unsigned s = shaders[static_cast<std::size_t>(idx)];
                                int shaderType = 0;
                                gl.glGetShaderiv(s, SHADER_TYPE, &shaderType);
                                const auto preview = read_shader_source_preview(gl, s);
                                const auto sName = game::extract_shader_name(
                                    preview, shaderType == VERTEX_SHADER ? "vp" : "fp");
                                if (shaderType == VERTEX_SHADER)        vertexShaderName   = sName;
                                else if (shaderType == FRAGMENT_SHADER) fragmentShaderName = sName;
                                // Retroactive archival: programs compiled before our hooks
                                // installed only ever pass through here, never the link detours.
                                maybe_dump_engine_shader(gl, s, sName);
                                LOG_INFO("GL program attached shader: program={} shader={} type={} preview={}",
                                         program, s, shader_type_name(shaderType), preview);
                            }
                            if (!vertexShaderName.empty() || !fragmentShaderName.empty()) {
                                const auto inferredSlot = infer_program_slot(vertexShaderName, fragmentShaderName);
                                {
                                    std::lock_guard lock(g_probeMutex);
                                    auto &record = g_programRecords[program];
                                    record.inferredSlot       = inferredSlot;
                                    record.vertexShaderName   = vertexShaderName;
                                    record.fragmentShaderName = fragmentShaderName;
                                }
                                if (inferredSlot) {
                                    LOG_INFO("GL program slot inference: program={} slot={} vertex={} fragment={}",
                                             program, *inferredSlot, vertexShaderName, fragmentShaderName);
                                } else {
                                    LOG_INFO("GL program slot inference: program={} slot=<unknown> vertex={} fragment={}",
                                             program,
                                             vertexShaderName.empty()   ? "<unknown>" : vertexShaderName,
                                             fragmentShaderName.empty() ? "<unknown>" : fragmentShaderName);
                                }
                            }
                        }
                    }
                }
            }
        }

        static void APIENTRY detour_glUseProgramObjectARB(unsigned program) {
            const auto callerInfo = current_caller_info();
            g_glUseProgramObjectARBHook.original()(program);

            // Uniform feed (post-original, program is now bound)
            if (program != 0) {
                bool isOverridden = false;
                {
                    std::lock_guard lock(g_probeMutex);
                    isOverridden = g_overriddenPrograms.contains(program);
                }
                if (isOverridden) {
                    feed_uniforms_to_program(program);
                }
            }

            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto &record = g_programRecords[program];
                shouldLog = !record.callerLogged.contains(callerInfo.caller);
                record.useLogged = true;
                record.callerLogged.emplace(callerInfo.caller, true);
            }

            if (shouldLog) {
                LOG_INFO("GL program bind (ARB): program={} {}",
                         program, caller_summary(callerInfo));

                if (program != 0) {
                    const auto &gl = game::gl::get_gl_functions();
                    if (gl.glGetProgramiv && gl.glGetAttachedShaders && gl.glGetShaderiv) {
                        int attachedShaderCount = 0;
                        gl.glGetProgramiv(program, ATTACHED_SHADERS, &attachedShaderCount);
                        if (attachedShaderCount > 0) {
                            std::vector<unsigned> shaders(static_cast<std::size_t>(attachedShaderCount), 0u);
                            int actualShaderCount = 0;
                            gl.glGetAttachedShaders(program, attachedShaderCount,
                                                    &actualShaderCount, shaders.data());
                            std::string vertexShaderName;
                            std::string fragmentShaderName;
                            for (int idx = 0; idx < actualShaderCount; ++idx) {
                                const unsigned s = shaders[static_cast<std::size_t>(idx)];
                                int shaderType = 0;
                                gl.glGetShaderiv(s, SHADER_TYPE, &shaderType);
                                const auto preview = read_shader_source_preview(gl, s);
                                const auto sName = game::extract_shader_name(
                                    preview, shaderType == VERTEX_SHADER ? "vp" : "fp");
                                if (shaderType == VERTEX_SHADER)        vertexShaderName   = sName;
                                else if (shaderType == FRAGMENT_SHADER) fragmentShaderName = sName;
                                // Retroactive archival (see non-ARB path note).
                                maybe_dump_engine_shader(gl, s, sName);
                                LOG_INFO("GL program attached shader (ARB): program={} shader={} type={} preview={}",
                                         program, s, shader_type_name(shaderType), preview);
                            }
                            if (!vertexShaderName.empty() || !fragmentShaderName.empty()) {
                                const auto inferredSlot = infer_program_slot(vertexShaderName, fragmentShaderName);
                                {
                                    std::lock_guard lock(g_probeMutex);
                                    auto &record = g_programRecords[program];
                                    record.inferredSlot       = inferredSlot;
                                    record.vertexShaderName   = vertexShaderName;
                                    record.fragmentShaderName = fragmentShaderName;
                                }
                                if (inferredSlot) {
                                    LOG_INFO("GL program slot inference (ARB): program={} slot={} vertex={} fragment={}",
                                             program, *inferredSlot, vertexShaderName, fragmentShaderName);
                                } else {
                                    LOG_INFO("GL program slot inference (ARB): program={} slot=<unknown> vertex={} fragment={}",
                                             program,
                                             vertexShaderName.empty()   ? "<unknown>" : vertexShaderName,
                                             fragmentShaderName.empty() ? "<unknown>" : fragmentShaderName);
                                }
                            }
                        }
                    }
                }
            }
        }
        // endregion

    } // anonymous namespace

    // region Public API

    ShaderRuntimeCapabilities detect_shader_runtime_capabilities() noexcept {
        const auto &gl = game::gl::get_gl_functions();
        ShaderRuntimeCapabilities caps{};
        caps.baseGlReady                  = gl.valid;
        caps.shaderObjectsAvailable       = gl.shaderObjectsAvailable;
        caps.shaderIntrospectionAvailable = gl.shaderIntrospectionAvailable;
        caps.uniformApiAvailable          = gl.uniformApiAvailable;
        caps.readyForSourcePatching       = gl.shaderObjectsAvailable &&
                                            gl.shaderIntrospectionAvailable &&
                                            gl.uniformApiAvailable;
        caps.glVersion    = get_gl_string(gl, game::gl::VERSION);
        caps.glslVersion  = get_gl_string(gl, game::gl::SHADING_LANGUAGE_VERSION);
        caps.glVendor     = get_gl_string(gl, game::gl::VENDOR);
        caps.glRenderer   = get_gl_string(gl, game::gl::RENDERER);
        return caps;
    }

    void log_shader_runtime_capabilities() noexcept {
        const auto caps = detect_shader_runtime_capabilities();
        LOG_INFO("Shader runtime readiness:");
        LOG_INFO("  Base GL API: {}",           caps.baseGlReady ? "ready" : "missing required functions");
        LOG_INFO("  Shader objects: {}",         caps.shaderObjectsAvailable ? "ready" : "missing");
        LOG_INFO("  Shader introspection: {}",   caps.shaderIntrospectionAvailable ? "ready" : "missing");
        LOG_INFO("  Uniform API: {}",            caps.uniformApiAvailable ? "ready" : "missing");
        LOG_INFO("  Source patching readiness: {}",
                 caps.readyForSourcePatching ? "ready" : "not ready");
        LOG_INFO("  ARB shader API: {}",
                 game::gl::get_gl_functions().arbShaderObjectsAvailable ? "ready" : "missing");
        if (!caps.glVersion.empty())   LOG_INFO("  GL version: {}",  caps.glVersion);
        if (!caps.glslVersion.empty()) LOG_INFO("  GLSL version: {}", caps.glslVersion);
        if (!caps.glVendor.empty())    LOG_INFO("  GL vendor: {}",   caps.glVendor);
        if (!caps.glRenderer.empty())  LOG_INFO("  GL renderer: {}", caps.glRenderer);
    }

    namespace {
        // region V5 gate probe: does the engine ever bind FBOs itself?
        std::set<unsigned long long> g_fboBindsLogged;

        void APIENTRY detour_glBindFramebuffer(unsigned target, unsigned framebuffer) {
            if (framebuffer != 0) {
                const auto key = (static_cast<unsigned long long>(target) << 32) | framebuffer;
                bool shouldLog = false;
                {
                    std::lock_guard lock(g_probeMutex);
                    shouldLog = g_fboBindsLogged.insert(key).second;
                }
                if (shouldLog) {
                    LOG_WARN("V5: engine bound FBO {} on target 0x{:X}", framebuffer, target);
                }
            }
            g_glBindFramebufferHook.original()(target, framebuffer);
        }
        // endregion
    }

    bool install_shader_probes(const core::EngineConfig &cfg) noexcept {
        std::lock_guard lock(g_probeMutex);
        if (g_shaderProbesInstalled) return true;

        g_cfg = cfg;

        configure_magenta_shaders(cfg.debugMagentaShaders);
        if (!g_magentaShaders.empty()) {
            std::string joined;
            for (const auto &s : g_magentaShaders) {
                if (!joined.empty()) joined += ',';
                joined += s;
            }
            LOG_INFO("Magenta debug shaders: {}", joined);
        }

        // Derive DLL directory for override dir and dump dir
#ifdef _WIN64
        {
            wchar_t wpath[MAX_PATH]{};
            HMODULE selfModule = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&install_shader_probes),
                               &selfModule);
            GetModuleFileNameW(selfModule ? selfModule : GetModuleHandleW(nullptr), wpath, MAX_PATH);
            const std::filesystem::path dllPath(wpath);
            const auto dllDir = dllPath.parent_path();

            // Override registry
            const auto overrideDir = dllDir / cfg.shaderOverrideDir;
            g_registry.load_from_directory(overrideDir);
            LOG_INFO("Shader override registry: {} shader(s) loaded from {}",
                     g_registry.size(), overrideDir.string());

            // Dump directory
            g_dumpDir = dllDir / "iee-shader-dumps";
        }
#else
        {
            // macOS/Linux build path (host tests only — GL hooks never run)
            const std::filesystem::path cwd = std::filesystem::current_path();
            const auto overrideDir = cwd / cfg.shaderOverrideDir;
            g_registry.load_from_directory(overrideDir);
            g_dumpDir = cwd / "iee-shader-dumps";
        }
#endif

        const auto &gl = game::gl::get_gl_functions();
        if (!gl.readyForSourcePatching &&
            !gl.arbShaderObjectsAvailable &&
            !(gl.shaderObjectsAvailable && gl.shaderIntrospectionAvailable)) {
            return false;
        }

        try {
            if (gl.glShaderSource) {
                g_glShaderSourceHook.create(reinterpret_cast<void *>(gl.glShaderSource),
                                            reinterpret_cast<void *>(&detour_glShaderSource));
                g_glShaderSourceHook.enable();
            }
            if (gl.glCompileShader) {
                g_glCompileShaderHook.create(reinterpret_cast<void *>(gl.glCompileShader),
                                             reinterpret_cast<void *>(&detour_glCompileShader));
                g_glCompileShaderHook.enable();
            }
            if (gl.glLinkProgram) {
                g_glLinkProgramHook.create(reinterpret_cast<void *>(gl.glLinkProgram),
                                           reinterpret_cast<void *>(&detour_glLinkProgram));
                g_glLinkProgramHook.enable();
            }
            if (gl.glUseProgram) {
                g_glUseProgramHook.create(reinterpret_cast<void *>(gl.glUseProgram),
                                          reinterpret_cast<void *>(&detour_glUseProgram));
                g_glUseProgramHook.enable();
            }
            if (gl.glShaderSourceARB) {
                g_glShaderSourceARBHook.create(reinterpret_cast<void *>(gl.glShaderSourceARB),
                                               reinterpret_cast<void *>(&detour_glShaderSourceARB));
                g_glShaderSourceARBHook.enable();
            }
            if (gl.glCompileShaderARB) {
                g_glCompileShaderARBHook.create(reinterpret_cast<void *>(gl.glCompileShaderARB),
                                                reinterpret_cast<void *>(&detour_glCompileShaderARB));
                g_glCompileShaderARBHook.enable();
            }
            if (gl.glLinkProgramARB) {
                g_glLinkProgramARBHook.create(reinterpret_cast<void *>(gl.glLinkProgramARB),
                                              reinterpret_cast<void *>(&detour_glLinkProgramARB));
                g_glLinkProgramARBHook.enable();
            }
            if (gl.glUseProgramObjectARB) {
                g_glUseProgramObjectARBHook.create(reinterpret_cast<void *>(gl.glUseProgramObjectARB),
                                                   reinterpret_cast<void *>(&detour_glUseProgramObjectARB));
                g_glUseProgramObjectARBHook.enable();
            }
            if (gl.glBindFramebuffer) {
                g_glBindFramebufferHook.create(reinterpret_cast<void *>(gl.glBindFramebuffer),
                                               reinterpret_cast<void *>(&detour_glBindFramebuffer));
                g_glBindFramebufferHook.enable();
            }
        } catch (const std::exception &e) {
            LOG_WARN("Failed to install GL shader probes: {}", e.what());
            g_glBindFramebufferHook.disable();
            g_glUseProgramObjectARBHook.disable();
            g_glLinkProgramARBHook.disable();
            g_glCompileShaderARBHook.disable();
            g_glShaderSourceARBHook.disable();
            g_glUseProgramHook.disable();
            g_glLinkProgramHook.disable();
            g_glCompileShaderHook.disable();
            g_glShaderSourceHook.disable();
            return false;
        }

        g_shaderProbesInstalled = true;
        LOG_INFO("Installed GL shader probes");
        return true;
    }

    void uninstall_shader_probes() noexcept {
        std::lock_guard lock(g_probeMutex);
        g_glBindFramebufferHook.disable();
        g_glUseProgramObjectARBHook.disable();
        g_glLinkProgramARBHook.disable();
        g_glCompileShaderARBHook.disable();
        g_glShaderSourceARBHook.disable();
        g_glUseProgramHook.disable();
        g_glLinkProgramHook.disable();
        g_glCompileShaderHook.disable();
        g_glShaderSourceHook.disable();
        g_shaderRecords.clear();
        g_programRecords.clear();
        g_overriddenPrograms.clear();
        g_pendingFallback.clear();
        g_dumpedShaders.clear();
        g_fboBindsLogged.clear();
        g_shaderProbesInstalled = false;
    }

    void on_frame_tick(float secondsSinceStart) noexcept {
        g_uniformTime.store(secondsSinceStart, std::memory_order_relaxed);

        if (!g_cfg.enableDebugHotkeys) {
            return;
        }

        // F10: toggle the visual effect of shader overrides (uIeeEnabled), edge-triggered.
        static bool f10WasDown = false;
        const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10Down && !f10WasDown) {
            const bool enabled = !g_overridesEnabled.load(std::memory_order_relaxed);
            g_overridesEnabled.store(enabled, std::memory_order_relaxed);
            LOG_INFO("Hotkey F10: shader override effect {}", enabled ? "ON" : "OFF");
        }
        f10WasDown = f10Down;
    }

    void set_override_effect_enabled(bool enabled) noexcept {
        g_overridesEnabled.store(enabled, std::memory_order_relaxed);
    }

    bool override_effect_enabled() noexcept {
        return g_overridesEnabled.load(std::memory_order_relaxed);
    }

    // endregion

} // namespace iee::probe
