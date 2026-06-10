#include "shader_runtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include <intrin.h>
#include <windows.h>
#include <dbghelp.h>
#include <spdlog/fmt/fmt.h>

#include "iee/core/hooking.h"
#include "iee/core/pattern_scanner.h"
#include "opengl_types.h"
#include "iee/core/logger.h"

namespace iee::game {
    namespace {
        constexpr unsigned SHADER_TYPE = 0x8B4F;
        constexpr unsigned ATTACHED_SHADERS = 0x8B85;
        constexpr unsigned COMPILE_STATUS = 0x8B81;
        constexpr unsigned LINK_STATUS = 0x8B82;
        constexpr unsigned INFO_LOG_LENGTH = 0x8B84;
        constexpr unsigned VERTEX_SHADER = 0x8B31;
        constexpr unsigned FRAGMENT_SHADER = 0x8B30;

        struct ShaderRecord {
            std::string sourcePreview;
            std::size_t sourceBytes{};
            bool sawShaderSource{};
            bool compileLogged{};
        };

        struct ProgramRecord {
            bool linkLogged{};
            bool useLogged{};
            std::unordered_map<std::uintptr_t, bool> callerLogged;
            std::optional<int> inferredSlot;
            std::string vertexShaderName;
            std::string fragmentShaderName;
        };

        struct CallerInfo {
            std::uintptr_t caller{};
            std::optional<std::uintptr_t> callerRva;
            std::string moduleName;
            std::string modulePath;
            std::string symbolName;
            std::uint64_t symbolDisplacement{};
        };

        using Fn_glShaderSource = void (APIENTRY*)(unsigned, int, const char* const*, const int*);
        using Fn_glCompileShader = void (APIENTRY*)(unsigned);
        using Fn_glLinkProgram = void (APIENTRY*)(unsigned);
        using Fn_glUseProgram = void (APIENTRY*)(unsigned);
        using Fn_glShaderSourceARB = void (APIENTRY*)(unsigned, int, const char* const*, const int*);
        using Fn_glCompileShaderARB = void (APIENTRY*)(unsigned);
        using Fn_glLinkProgramARB = void (APIENTRY*)(unsigned);
        using Fn_glUseProgramObjectARB = void (APIENTRY*)(unsigned);

        core::Hook<Fn_glShaderSource> g_glShaderSourceHook;
        core::Hook<Fn_glCompileShader> g_glCompileShaderHook;
        core::Hook<Fn_glLinkProgram> g_glLinkProgramHook;
        core::Hook<Fn_glUseProgram> g_glUseProgramHook;
        core::Hook<Fn_glShaderSourceARB> g_glShaderSourceARBHook;
        core::Hook<Fn_glCompileShaderARB> g_glCompileShaderARBHook;
        core::Hook<Fn_glLinkProgramARB> g_glLinkProgramARBHook;
        core::Hook<Fn_glUseProgramObjectARB> g_glUseProgramObjectARBHook;

        std::mutex g_probeMutex;
        std::unordered_map<unsigned, ShaderRecord> g_shaderRecords;
        std::unordered_map<unsigned, ProgramRecord> g_programRecords;
        bool g_shaderProbesInstalled = false;
        thread_local ShaderRenderContext g_renderContext{};
        std::set<std::string, std::less<>> g_magentaShaders;

        std::string sanitize_preview(std::string text) {
            for (char& ch : text) {
                if (ch == '\n' || ch == '\r' || ch == '\t') {
                    ch = ' ';
                }
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
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
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

        std::string gather_shader_source_preview(int count, const char* const* strings, const int* lengths) {
            if (count <= 0 || !strings) {
                return {};
            }

            std::string combined;
            combined.reserve(256);

            for (int index = 0; index < count; ++index) {
                const char* chunk = strings[index];
                if (!chunk) {
                    continue;
                }

                std::size_t chunkSize = 0;
                if (lengths && lengths[index] >= 0) {
                    chunkSize = static_cast<std::size_t>(lengths[index]);
                } else {
                    chunkSize = std::char_traits<char>::length(chunk);
                }

                combined.append(chunk, chunkSize);
                if (combined.size() >= 240) {
                    break;
                }
            }

            return sanitize_preview(std::move(combined));
        }

        std::size_t gather_shader_source_size(int count, const char* const* strings, const int* lengths) {
            if (count <= 0 || !strings) {
                return 0;
            }

            std::size_t total = 0;
            for (int index = 0; index < count; ++index) {
                const char* chunk = strings[index];
                if (!chunk) {
                    continue;
                }

                if (lengths && lengths[index] >= 0) {
                    total += static_cast<std::size_t>(lengths[index]);
                } else {
                    total += std::char_traits<char>::length(chunk);
                }
            }
            return total;
        }

        const char* shader_type_name(int shaderType) noexcept {
            switch (shaderType) {
                case VERTEX_SHADER:
                    return "vertex";
                case FRAGMENT_SHADER:
                    return "fragment";
                default:
                    return "unknown";
            }
        }

        std::string read_shader_log(const gl::OpenGLFunctions& gl, unsigned shader) {
            if (!gl.glGetShaderiv || !gl.glGetShaderInfoLog) {
                return {};
            }

            int infoLogLength = 0;
            gl.glGetShaderiv(shader, INFO_LOG_LENGTH, &infoLogLength);
            if (infoLogLength <= 1) {
                return {};
            }

            std::string infoLog(static_cast<std::size_t>(infoLogLength), '\0');
            int written = 0;
            gl.glGetShaderInfoLog(shader, infoLogLength, &written, infoLog.data());
            if (written > 0 && written < infoLogLength) {
                infoLog.resize(static_cast<std::size_t>(written));
            }
            return sanitize_preview(std::move(infoLog));
        }

        std::string basename(std::string path) {
            const auto pos = path.find_last_of("\\/");
            if (pos == std::string::npos) {
                return path;
            }
            return path.substr(pos + 1);
        }

        std::string read_shader_source_preview(const gl::OpenGLFunctions& gl, unsigned shader) {
            if (!gl.glGetShaderSource) {
                return {};
            }

            constexpr int kMaxSourceBytes = 1024;
            std::array<char, kMaxSourceBytes> buffer{};
            int written = 0;
            gl.glGetShaderSource(shader, kMaxSourceBytes, &written, buffer.data());
            if (written <= 0) {
                return {};
            }

            return sanitize_preview(std::string(buffer.data(), static_cast<std::size_t>(written)));
        }

        std::string read_program_log(const gl::OpenGLFunctions& gl, unsigned program) {
            if (!gl.glGetProgramiv || !gl.glGetProgramInfoLog) {
                return {};
            }

            int infoLogLength = 0;
            gl.glGetProgramiv(program, INFO_LOG_LENGTH, &infoLogLength);
            if (infoLogLength <= 1) {
                return {};
            }

            std::string infoLog(static_cast<std::size_t>(infoLogLength), '\0');
            int written = 0;
            gl.glGetProgramInfoLog(program, infoLogLength, &written, infoLog.data());
            if (written > 0 && written < infoLogLength) {
                infoLog.resize(static_cast<std::size_t>(written));
            }
            return sanitize_preview(std::move(infoLog));
        }

        std::string_view get_gl_string(const gl::OpenGLFunctions& gl, unsigned name) noexcept {
            if (!gl.glGetString) {
                return {};
            }

            const auto* value = gl.glGetString(name);
            if (!value) {
                return {};
            }

            return reinterpret_cast<const char*>(value);
        }

        CallerInfo current_caller_info() noexcept {
            CallerInfo info{};
            info.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            const auto module = core::get_module_span(nullptr);
            if (module) {
                const auto moduleBase = reinterpret_cast<std::uintptr_t>(module->base);
                const auto moduleEnd = moduleBase + module->size;
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
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = MAX_SYM_NAME;

                DWORD64 displacement = 0;
                if (SymFromAddr(GetCurrentProcess(), static_cast<DWORD64>(info.caller), &displacement, symbol)) {
                    info.symbolName.assign(symbol->Name, symbol->NameLen);
                    info.symbolDisplacement = displacement;
                }
            }

            return info;
        }

        std::string caller_summary(const CallerInfo& callerInfo) {
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

        std::string extract_shader_name(std::string_view preview, std::string_view prefix) {
            const auto commentPos = preview.find("// ");
            if (commentPos == std::string_view::npos) {
                return {};
            }

            const auto nameStart = commentPos + 3;
            const auto endPos = preview.find(".glsl", nameStart);
            if (endPos == std::string_view::npos || endPos <= nameStart) {
                return {};
            }

            const auto name = preview.substr(nameStart, endPos - nameStart);
            if (!prefix.empty() && name.rfind(prefix, 0) != 0) {
                return {};
            }
            return std::string(name);
        }

        std::size_t find_main_body_open(std::string_view source) {
            constexpr std::string_view patterns[] = {"void main(", "void main ("};
            for (const auto pattern : patterns) {
                const auto mainPos = source.find(pattern);
                if (mainPos == std::string_view::npos) continue;
                const auto bracePos = source.find('{', mainPos);
                if (bracePos != std::string_view::npos) {
                    return bracePos;
                }
            }
            return std::string_view::npos;
        }

        std::string maybe_patch_magenta_shader(std::string_view sourcePreview,
                                               int count,
                                               const char* const* strings,
                                               const int* lengths) {
            if (count != 1 || !strings || !strings[0]) {
                return {};
            }

            const auto shaderName = extract_shader_name(sourcePreview, "fp");
            if (shaderName.empty()) {
                return {};
            }
            if (!g_magentaShaders.contains(upper_copy(shaderName))) {
                return {};
            }

            std::size_t sourceLength = 0;
            if (lengths && lengths[0] >= 0) {
                sourceLength = static_cast<std::size_t>(lengths[0]);
            } else {
                sourceLength = std::char_traits<char>::length(strings[0]);
            }

            std::string patched(strings[0], sourceLength);
            if (patched.find("IEE_DEBUG_MAGENTA") != std::string::npos) {
                return {};
            }

            const auto bodyOpen = find_main_body_open(patched);
            if (bodyOpen == std::string::npos) {
                return {};
            }

            constexpr std::string_view injection =
                "\n/* IEE_DEBUG_MAGENTA */\n"
                "gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n"
                "return;\n";
            patched.insert(bodyOpen + 1, injection);
            LOG_INFO("Applied magenta debug patch to shader {}", shaderName);
            return patched;
        }

        std::optional<int> infer_program_slot(std::string_view vertexShaderName, std::string_view fragmentShaderName) {
            if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpDraw") return 0;
            if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpTone") return 1;
            if (vertexShaderName == "vpBlit" && fragmentShaderName == "fpCatRom") return 2;
            if ((vertexShaderName == "vpYUV" || vertexShaderName == "vpDraw") && fragmentShaderName == "fpYUV") return 3;
            if (vertexShaderName == "vpYUV" && fragmentShaderName == "fpYUVGRY") return 4;
            if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpSprite") return 5;
            if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpFONT") return 6;
            if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpSELECT") return 7;
            if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpSEAM") return 8;
            return std::nullopt;
        }

        void APIENTRY detour_glShaderSource(unsigned shader, int count, const char* const* string, const int* length) {
            const auto preview = gather_shader_source_preview(count, string, length);
            const auto patchedSource = maybe_patch_magenta_shader(preview, count, string, length);
            const char* patchedPtr = nullptr;
            int patchedLength = 0;
            const char* const* sourceStrings = string;
            const int* sourceLengths = length;
            if (!patchedSource.empty()) {
                patchedPtr = patchedSource.c_str();
                patchedLength = static_cast<int>(patchedSource.size());
                sourceStrings = &patchedPtr;
                sourceLengths = &patchedLength;
            }
            {
                std::lock_guard lock(g_probeMutex);
                auto& record = g_shaderRecords[shader];
                record.sourceBytes = gather_shader_source_size(patchedSource.empty() ? count : 1,
                                                              sourceStrings,
                                                              sourceLengths);
                record.sourcePreview = gather_shader_source_preview(patchedSource.empty() ? count : 1,
                                                                   sourceStrings,
                                                                   sourceLengths);
                record.sawShaderSource = true;
                record.compileLogged = false;
            }

            g_glShaderSourceHook.original()(shader,
                                           patchedSource.empty() ? count : 1,
                                           sourceStrings,
                                           sourceLengths);
        }

        void APIENTRY detour_glShaderSourceARB(unsigned shader, int count, const char* const* string, const int* length) {
            const auto preview = gather_shader_source_preview(count, string, length);
            const auto patchedSource = maybe_patch_magenta_shader(preview, count, string, length);
            const char* patchedPtr = nullptr;
            int patchedLength = 0;
            const char* const* sourceStrings = string;
            const int* sourceLengths = length;
            if (!patchedSource.empty()) {
                patchedPtr = patchedSource.c_str();
                patchedLength = static_cast<int>(patchedSource.size());
                sourceStrings = &patchedPtr;
                sourceLengths = &patchedLength;
            }
            {
                std::lock_guard lock(g_probeMutex);
                auto& record = g_shaderRecords[shader];
                record.sourceBytes = gather_shader_source_size(patchedSource.empty() ? count : 1,
                                                              sourceStrings,
                                                              sourceLengths);
                record.sourcePreview = gather_shader_source_preview(patchedSource.empty() ? count : 1,
                                                                   sourceStrings,
                                                                   sourceLengths);
                record.sawShaderSource = true;
                record.compileLogged = false;
            }

            g_glShaderSourceARBHook.original()(shader,
                                              patchedSource.empty() ? count : 1,
                                              sourceStrings,
                                              sourceLengths);
        }

        void APIENTRY detour_glCompileShader(unsigned shader) {
            g_glCompileShaderHook.original()(shader);

            const auto& gl = gl::get_gl_functions();
            int shaderType = 0;
            int compileStatus = 0;
            if (gl.glGetShaderiv) {
                gl.glGetShaderiv(shader, SHADER_TYPE, &shaderType);
                gl.glGetShaderiv(shader, COMPILE_STATUS, &compileStatus);
            }

            std::string preview;
            std::size_t sourceBytes = 0;
            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto& record = g_shaderRecords[shader];
                shouldLog = !record.compileLogged;
                record.compileLogged = true;
                preview = record.sourcePreview;
                sourceBytes = record.sourceBytes;
            }

            if (!shouldLog) {
                return;
            }

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

        void APIENTRY detour_glCompileShaderARB(unsigned shader) {
            g_glCompileShaderARBHook.original()(shader);

            const auto& gl = gl::get_gl_functions();
            int shaderType = 0;
            int compileStatus = 0;
            if (gl.glGetShaderiv) {
                gl.glGetShaderiv(shader, SHADER_TYPE, &shaderType);
                gl.glGetShaderiv(shader, COMPILE_STATUS, &compileStatus);
            }

            std::string preview;
            std::size_t sourceBytes = 0;
            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto& record = g_shaderRecords[shader];
                shouldLog = !record.compileLogged;
                record.compileLogged = true;
                preview = record.sourcePreview;
                sourceBytes = record.sourceBytes;
            }

            if (!shouldLog) {
                return;
            }

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

        void APIENTRY detour_glLinkProgram(unsigned program) {
            g_glLinkProgramHook.original()(program);

            const auto& gl = gl::get_gl_functions();
            int linkStatus = 0;
            if (gl.glGetProgramiv) {
                gl.glGetProgramiv(program, LINK_STATUS, &linkStatus);
            }

            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto& record = g_programRecords[program];
                shouldLog = !record.linkLogged;
                record.linkLogged = true;
            }

            if (!shouldLog) {
                return;
            }

            const auto infoLog = read_program_log(gl, program);
            LOG_INFO("GL program link: program={} status={}",
                     program,
                     linkStatus != 0 ? "ok" : "fail");
            if (!infoLog.empty()) {
                LOG_INFO("GL program link log: program={} log={}", program, infoLog);
            }
        }

        void APIENTRY detour_glLinkProgramARB(unsigned program) {
            g_glLinkProgramARBHook.original()(program);

            const auto& gl = gl::get_gl_functions();
            int linkStatus = 0;
            if (gl.glGetProgramiv) {
                gl.glGetProgramiv(program, LINK_STATUS, &linkStatus);
            }

            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto& record = g_programRecords[program];
                shouldLog = !record.linkLogged;
                record.linkLogged = true;
            }

            if (!shouldLog) {
                return;
            }

            const auto infoLog = read_program_log(gl, program);
            LOG_INFO("GL program link (ARB): program={} status={}",
                     program,
                     linkStatus != 0 ? "ok" : "fail");
            if (!infoLog.empty()) {
                LOG_INFO("GL program link log (ARB): program={} log={}", program, infoLog);
            }
        }

        void APIENTRY detour_glUseProgram(unsigned program) {
            const auto callerInfo = current_caller_info();
            g_glUseProgramHook.original()(program);

            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto& record = g_programRecords[program];
                shouldLog = !record.callerLogged.contains(callerInfo.caller);
                record.useLogged = true;
                record.callerLogged.emplace(callerInfo.caller, true);
            }

            if (shouldLog) {
                if (g_renderContext.active) {
                    LOG_INFO("GL program bind: program={} {} source=RenderTexture texId={} flags=0x{:X} tone={} area={}",
                             program,
                             caller_summary(callerInfo),
                             g_renderContext.texId,
                             g_renderContext.flags,
                             g_renderContext.tone,
                             g_renderContext.areaResref.empty() ? "<unknown>" : g_renderContext.areaResref);
                } else {
                    LOG_INFO("GL program bind: program={} {} source=<outside RenderTexture>",
                             program,
                             caller_summary(callerInfo));
                }
                if (program != 0) {
                    const auto& gl = gl::get_gl_functions();
                    if (gl.glGetProgramiv && gl.glGetAttachedShaders && gl.glGetShaderiv) {
                        int attachedShaderCount = 0;
                        gl.glGetProgramiv(program, ATTACHED_SHADERS, &attachedShaderCount);
                        if (attachedShaderCount > 0) {
                            std::vector<unsigned> shaders(static_cast<std::size_t>(attachedShaderCount), 0);
                            int actualShaderCount = 0;
                            gl.glGetAttachedShaders(program,
                                                    attachedShaderCount,
                                                    &actualShaderCount,
                                                    shaders.data());
                            std::string vertexShaderName;
                            std::string fragmentShaderName;
                            for (int index = 0; index < actualShaderCount; ++index) {
                                int shaderType = 0;
                                gl.glGetShaderiv(shaders[static_cast<std::size_t>(index)], SHADER_TYPE, &shaderType);
                                const auto preview = read_shader_source_preview(gl, shaders[static_cast<std::size_t>(index)]);
                                const auto shaderName = extract_shader_name(preview, shaderType == VERTEX_SHADER ? "vp" : "fp");
                                if (shaderType == VERTEX_SHADER) {
                                    vertexShaderName = shaderName;
                                } else if (shaderType == FRAGMENT_SHADER) {
                                    fragmentShaderName = shaderName;
                                }
                                LOG_INFO("GL program attached shader: program={} shader={} type={} preview={}",
                                         program,
                                         shaders[static_cast<std::size_t>(index)],
                                         shader_type_name(shaderType),
                                         preview);
                            }

                            if (!vertexShaderName.empty() || !fragmentShaderName.empty()) {
                                const auto inferredSlot = infer_program_slot(vertexShaderName, fragmentShaderName);
                                {
                                    std::lock_guard lock(g_probeMutex);
                                    auto& record = g_programRecords[program];
                                    record.inferredSlot = inferredSlot;
                                    record.vertexShaderName = vertexShaderName;
                                    record.fragmentShaderName = fragmentShaderName;
                                }
                                if (inferredSlot) {
                                    LOG_INFO("GL program slot inference: program={} slot={} vertex={} fragment={}",
                                             program,
                                             *inferredSlot,
                                             vertexShaderName,
                                             fragmentShaderName);
                                } else {
                                    LOG_INFO("GL program slot inference: program={} slot=<unknown> vertex={} fragment={}",
                                             program,
                                             vertexShaderName.empty() ? "<unknown>" : vertexShaderName,
                                             fragmentShaderName.empty() ? "<unknown>" : fragmentShaderName);
                                }
                            }
                        }
                    }
                }
            }
        }

        void APIENTRY detour_glUseProgramObjectARB(unsigned program) {
            const auto callerInfo = current_caller_info();
            g_glUseProgramObjectARBHook.original()(program);

            bool shouldLog = false;
            {
                std::lock_guard lock(g_probeMutex);
                auto& record = g_programRecords[program];
                shouldLog = !record.callerLogged.contains(callerInfo.caller);
                record.useLogged = true;
                record.callerLogged.emplace(callerInfo.caller, true);
            }

            if (shouldLog) {
                if (g_renderContext.active) {
                    LOG_INFO("GL program bind (ARB): program={} {} source=RenderTexture texId={} flags=0x{:X} tone={} area={}",
                             program,
                             caller_summary(callerInfo),
                             g_renderContext.texId,
                             g_renderContext.flags,
                             g_renderContext.tone,
                             g_renderContext.areaResref.empty() ? "<unknown>" : g_renderContext.areaResref);
                } else {
                    LOG_INFO("GL program bind (ARB): program={} {} source=<outside RenderTexture>",
                             program,
                             caller_summary(callerInfo));
                }
                if (program != 0) {
                    const auto& gl = gl::get_gl_functions();
                    if (gl.glGetProgramiv && gl.glGetAttachedShaders && gl.glGetShaderiv) {
                        int attachedShaderCount = 0;
                        gl.glGetProgramiv(program, ATTACHED_SHADERS, &attachedShaderCount);
                        if (attachedShaderCount > 0) {
                            std::vector<unsigned> shaders(static_cast<std::size_t>(attachedShaderCount), 0);
                            int actualShaderCount = 0;
                            gl.glGetAttachedShaders(program,
                                                    attachedShaderCount,
                                                    &actualShaderCount,
                                                    shaders.data());
                            std::string vertexShaderName;
                            std::string fragmentShaderName;
                            for (int index = 0; index < actualShaderCount; ++index) {
                                int shaderType = 0;
                                gl.glGetShaderiv(shaders[static_cast<std::size_t>(index)], SHADER_TYPE, &shaderType);
                                const auto preview = read_shader_source_preview(gl, shaders[static_cast<std::size_t>(index)]);
                                const auto shaderName = extract_shader_name(preview, shaderType == VERTEX_SHADER ? "vp" : "fp");
                                if (shaderType == VERTEX_SHADER) {
                                    vertexShaderName = shaderName;
                                } else if (shaderType == FRAGMENT_SHADER) {
                                    fragmentShaderName = shaderName;
                                }
                                LOG_INFO("GL program attached shader (ARB): program={} shader={} type={} preview={}",
                                         program,
                                         shaders[static_cast<std::size_t>(index)],
                                         shader_type_name(shaderType),
                                         preview);
                            }

                            if (!vertexShaderName.empty() || !fragmentShaderName.empty()) {
                                const auto inferredSlot = infer_program_slot(vertexShaderName, fragmentShaderName);
                                {
                                    std::lock_guard lock(g_probeMutex);
                                    auto& record = g_programRecords[program];
                                    record.inferredSlot = inferredSlot;
                                    record.vertexShaderName = vertexShaderName;
                                    record.fragmentShaderName = fragmentShaderName;
                                }
                                if (inferredSlot) {
                                    LOG_INFO("GL program slot inference (ARB): program={} slot={} vertex={} fragment={}",
                                             program,
                                             *inferredSlot,
                                             vertexShaderName,
                                             fragmentShaderName);
                                } else {
                                    LOG_INFO("GL program slot inference (ARB): program={} slot=<unknown> vertex={} fragment={}",
                                             program,
                                             vertexShaderName.empty() ? "<unknown>" : vertexShaderName,
                                             fragmentShaderName.empty() ? "<unknown>" : fragmentShaderName);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ShaderRuntimeCapabilities detect_shader_runtime_capabilities() noexcept {
        const auto& gl = gl::get_gl_functions();

        ShaderRuntimeCapabilities capabilities{};
        capabilities.baseGlReady = gl.valid;
        capabilities.shaderObjectsAvailable = gl.shaderObjectsAvailable;
        capabilities.shaderIntrospectionAvailable = gl.shaderIntrospectionAvailable;
        capabilities.uniformApiAvailable = gl.uniformApiAvailable;
        capabilities.readyForSourcePatching =
            gl.shaderObjectsAvailable &&
            gl.shaderIntrospectionAvailable &&
            gl.uniformApiAvailable;
        capabilities.glVersion = get_gl_string(gl, gl::VERSION);
        capabilities.glslVersion = get_gl_string(gl, gl::SHADING_LANGUAGE_VERSION);
        capabilities.glVendor = get_gl_string(gl, gl::VENDOR);
        capabilities.glRenderer = get_gl_string(gl, gl::RENDERER);
        return capabilities;
    }

    void log_shader_runtime_capabilities() noexcept {
        const auto capabilities = detect_shader_runtime_capabilities();

        LOG_INFO("Shader runtime readiness:");
        LOG_INFO("  Base GL API: {}", capabilities.baseGlReady ? "ready" : "missing required functions");
        LOG_INFO("  Shader objects: {}", capabilities.shaderObjectsAvailable ? "ready" : "missing");
        LOG_INFO("  Shader introspection: {}", capabilities.shaderIntrospectionAvailable ? "ready" : "missing");
        LOG_INFO("  Uniform API: {}", capabilities.uniformApiAvailable ? "ready" : "missing");
        LOG_INFO("  Source patching readiness: {}",
                 capabilities.readyForSourcePatching ? "ready" : "not ready");
        LOG_INFO("  ARB shader API: {}", gl::get_gl_functions().arbShaderObjectsAvailable ? "ready" : "missing");

        if (!capabilities.glVersion.empty()) {
            LOG_INFO("  GL version: {}", capabilities.glVersion);
        }
        if (!capabilities.glslVersion.empty()) {
            LOG_INFO("  GLSL version: {}", capabilities.glslVersion);
        }
        if (!capabilities.glVendor.empty()) {
            LOG_INFO("  GL vendor: {}", capabilities.glVendor);
        }
        if (!capabilities.glRenderer.empty()) {
            LOG_INFO("  GL renderer: {}", capabilities.glRenderer);
        }
    }

    bool install_shader_probes(const core::EngineConfig& cfg) noexcept {
        std::lock_guard lock(g_probeMutex);
        if (g_shaderProbesInstalled) {
            return true;
        }

        configure_magenta_shaders(cfg.debugMagentaShaders);
        if (!g_magentaShaders.empty()) {
            std::string joined;
            for (const auto& shader : g_magentaShaders) {
                if (!joined.empty()) joined += ",";
                joined += shader;
            }
            LOG_INFO("Magenta debug shaders: {}", joined);
        }

        const auto& gl = gl::get_gl_functions();
        if (!gl.readyForSourcePatching &&
            !gl.arbShaderObjectsAvailable &&
            !(gl.shaderObjectsAvailable && gl.shaderIntrospectionAvailable)) {
            return false;
        }

        try {
            if (gl.glShaderSource) {
                g_glShaderSourceHook.create(reinterpret_cast<void*>(gl.glShaderSource), reinterpret_cast<void*>(&detour_glShaderSource));
                g_glShaderSourceHook.enable();
            }
            if (gl.glCompileShader) {
                g_glCompileShaderHook.create(reinterpret_cast<void*>(gl.glCompileShader), reinterpret_cast<void*>(&detour_glCompileShader));
                g_glCompileShaderHook.enable();
            }
            if (gl.glLinkProgram) {
                g_glLinkProgramHook.create(reinterpret_cast<void*>(gl.glLinkProgram), reinterpret_cast<void*>(&detour_glLinkProgram));
                g_glLinkProgramHook.enable();
            }
            if (gl.glUseProgram) {
                g_glUseProgramHook.create(reinterpret_cast<void*>(gl.glUseProgram), reinterpret_cast<void*>(&detour_glUseProgram));
                g_glUseProgramHook.enable();
            }
            if (gl.glShaderSourceARB) {
                g_glShaderSourceARBHook.create(reinterpret_cast<void*>(gl.glShaderSourceARB), reinterpret_cast<void*>(&detour_glShaderSourceARB));
                g_glShaderSourceARBHook.enable();
            }
            if (gl.glCompileShaderARB) {
                g_glCompileShaderARBHook.create(reinterpret_cast<void*>(gl.glCompileShaderARB), reinterpret_cast<void*>(&detour_glCompileShaderARB));
                g_glCompileShaderARBHook.enable();
            }
            if (gl.glLinkProgramARB) {
                g_glLinkProgramARBHook.create(reinterpret_cast<void*>(gl.glLinkProgramARB), reinterpret_cast<void*>(&detour_glLinkProgramARB));
                g_glLinkProgramARBHook.enable();
            }
            if (gl.glUseProgramObjectARB) {
                g_glUseProgramObjectARBHook.create(reinterpret_cast<void*>(gl.glUseProgramObjectARB), reinterpret_cast<void*>(&detour_glUseProgramObjectARB));
                g_glUseProgramObjectARBHook.enable();
            }
        } catch (const std::exception& e) {
            LOG_WARN("Failed to install GL shader probes: {}", e.what());
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
        g_shaderProbesInstalled = false;
    }

    void set_shader_render_context(const ShaderRenderContext& context) noexcept {
        g_renderContext = context;
    }

    void clear_shader_render_context() noexcept {
        g_renderContext = {};
    }
}
