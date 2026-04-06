#include "shader_trace.h"

#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iee::game::shader_trace {
    namespace {
        using GLenum = unsigned int;
        using GLuint = unsigned int;
        using GLint = int;
        using GLsizei = int;
        using GLfloat = float;
        using GLchar = char;

        constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
        constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
        constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
        constexpr GLenum GL_LINK_STATUS = 0x8B82;
        constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
        constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
        constexpr GLenum GL_TEXTURE_WIDTH = 0x1000;
        constexpr GLenum GL_TEXTURE_HEIGHT = 0x1001;
        constexpr GLenum GL_TEXTURE0 = 0x84C0;

        using Fn_wglGetProcAddress = PROC (WINAPI*)(LPCSTR);
        using Fn_glBindTexture = void (APIENTRY*)(GLenum, GLuint);
        using Fn_glCreateShader = GLuint (APIENTRY*)(GLenum);
        using Fn_glDeleteShader = void (APIENTRY*)(GLuint);
        using Fn_glShaderSource = void (APIENTRY*)(GLuint, GLsizei, const GLchar *const *, const GLint *);
        using Fn_glCompileShader = void (APIENTRY*)(GLuint);
        using Fn_glGetShaderiv = void (APIENTRY*)(GLuint, GLenum, GLint *);
        using Fn_glGetShaderInfoLog = void (APIENTRY*)(GLuint, GLsizei, GLsizei *, GLchar *);
        using Fn_glCreateProgram = GLuint (APIENTRY*)();
        using Fn_glDeleteProgram = void (APIENTRY*)(GLuint);
        using Fn_glAttachShader = void (APIENTRY*)(GLuint, GLuint);
        using Fn_glLinkProgram = void (APIENTRY*)(GLuint);
        using Fn_glGetProgramiv = void (APIENTRY*)(GLuint, GLenum, GLint *);
        using Fn_glGetProgramInfoLog = void (APIENTRY*)(GLuint, GLsizei, GLsizei *, GLchar *);
        using Fn_glActiveTexture = void (APIENTRY*)(GLenum);
        using Fn_glGetTexLevelParameteriv = void (APIENTRY*)(GLenum, GLint, GLenum, GLint *);
        using Fn_glUseProgram = void (APIENTRY*)(GLuint);
        using Fn_glGetUniformLocation = GLint (APIENTRY*)(GLuint, const GLchar *);
        using Fn_glGetUniformfv = void (APIENTRY*)(GLuint, GLint, GLfloat *);
        using Fn_glGetUniformiv = void (APIENTRY*)(GLuint, GLint, GLint *);
        using Fn_glUniform1i = void (APIENTRY*)(GLint, GLint);
        using Fn_glUniform1f = void (APIENTRY*)(GLint, GLfloat);
        using Fn_glUniform2f = void (APIENTRY*)(GLint, GLfloat, GLfloat);
        using Fn_glUniform2fv = void (APIENTRY*)(GLint, GLsizei, const GLfloat *);
        using Fn_glUseProgramObjectARB = void (APIENTRY*)(GLuint);
        using Fn_glUniform1iARB = void (APIENTRY*)(GLint, GLint);
        using Fn_glUniform1fARB = void (APIENTRY*)(GLint, GLfloat);
        using Fn_glUniform2fARB = void (APIENTRY*)(GLint, GLfloat, GLfloat);
        using Fn_glUniform2fvARB = void (APIENTRY*)(GLint, GLsizei, const GLfloat *);

        struct ShaderRecord {
            GLenum type{};
            std::uint64_t sourceHash{};
            std::size_t sourceLength{};
            std::string label;
            bool hasTcScale{};
            bool hasBlurAmount{};
            bool hasSolidThreshold{};
            bool hasEasu{};
            bool compileSucceeded{};
        };

        struct ProgramRecord {
            std::vector<GLuint> attachedShaders;
            std::string label;
            bool interesting{};
            bool tcScaleInjectable{};
            bool linkSucceeded{};
            std::unordered_map<GLint, std::string> uniformNames;
            std::unordered_map<GLint, std::string> lastUniformValues;
            std::string lastInjectedTcScaleValue;
            GLuint lastInjectedTexture{};
        };

        core::Hook<Fn_glBindTexture> H_glBindTexture;
        core::Hook<Fn_wglGetProcAddress> H_wglGetProcAddress;

        std::mutex g_mutex;
        std::unordered_map<GLuint, ShaderRecord> g_shaders;
        std::unordered_map<GLuint, ProgramRecord> g_programs;
        std::unordered_map<GLint, GLuint> g_boundTextures2D;
        GLuint g_currentProgram = 0;
        GLuint g_lastLoggedProgram = 0;
        GLint g_currentActiveTextureUnit = 0;
        bool g_traceEnabled = false;
        bool g_tcScaleInjectionEnabled = false;
        bool g_enabled = false;
        bool g_installed = false;

        Fn_glBindTexture g_glBindTexture{};
        Fn_glCreateShader g_glCreateShader{};
        Fn_glDeleteShader g_glDeleteShader{};
        Fn_glShaderSource g_glShaderSource{};
        Fn_glCompileShader g_glCompileShader{};
        Fn_glGetShaderiv g_glGetShaderiv{};
        Fn_glGetShaderInfoLog g_glGetShaderInfoLog{};
        Fn_glCreateProgram g_glCreateProgram{};
        Fn_glDeleteProgram g_glDeleteProgram{};
        Fn_glAttachShader g_glAttachShader{};
        Fn_glLinkProgram g_glLinkProgram{};
        Fn_glGetProgramiv g_glGetProgramiv{};
        Fn_glGetProgramInfoLog g_glGetProgramInfoLog{};
        Fn_glActiveTexture g_glActiveTexture{};
        Fn_glGetTexLevelParameteriv g_glGetTexLevelParameteriv{};
        Fn_glUseProgram g_glUseProgram{};
        Fn_glGetUniformLocation g_glGetUniformLocation{};
        Fn_glGetUniformfv g_glGetUniformfv{};
        Fn_glGetUniformiv g_glGetUniformiv{};
        Fn_glUniform1i g_glUniform1i{};
        Fn_glUniform1f g_glUniform1f{};
        Fn_glUniform2f g_glUniform2f{};
        Fn_glUniform2fv g_glUniform2fv{};
        Fn_glUseProgramObjectARB g_glUseProgramObjectARB{};
        Fn_glUniform1iARB g_glUniform1iARB{};
        Fn_glUniform1fARB g_glUniform1fARB{};
        Fn_glUniform2fARB g_glUniform2fARB{};
        Fn_glUniform2fvARB g_glUniform2fvARB{};

        std::uint64_t fnv1a64(std::string_view value) noexcept {
            constexpr std::uint64_t offset = 14695981039346656037ull;
            constexpr std::uint64_t prime = 1099511628211ull;

            std::uint64_t hash = offset;
            for (const auto ch: value) {
                hash ^= static_cast<unsigned char>(ch);
                hash *= prime;
            }
            return hash;
        }

        std::string lower_copy(std::string_view value) {
            std::string out(value);
            std::transform(out.begin(),
                           out.end(),
                           out.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return out;
        }

        bool contains(std::string_view haystack, std::string_view needle) {
            return haystack.find(needle) != std::string_view::npos;
        }

        std::string collapse_whitespace(std::string_view source, std::size_t maxChars = 160) {
            std::string out;
            out.reserve(std::min(maxChars, source.size()));

            bool lastWasSpace = false;
            for (const auto ch: source) {
                const auto isSpace = std::isspace(static_cast<unsigned char>(ch)) != 0;
                if (isSpace) {
                    if (lastWasSpace || out.empty()) continue;
                    out.push_back(' ');
                    lastWasSpace = true;
                    if (out.size() >= maxChars) break;
                    continue;
                }

                out.push_back(static_cast<char>(ch));
                lastWasSpace = false;
                if (out.size() >= maxChars) break;
            }

            return out;
        }

        const char *shader_type_name(GLenum type) noexcept {
            switch (type) {
                case GL_FRAGMENT_SHADER: return "fragment";
                case GL_VERTEX_SHADER: return "vertex";
                default: return "other";
            }
        }

        PROC resolve_gl_proc(const char *name) noexcept {
            if (!name) return nullptr;

            static HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
            if (!opengl32) {
                opengl32 = LoadLibraryA("opengl32.dll");
            }
            if (!opengl32) return nullptr;

            if (auto proc = GetProcAddress(opengl32, name)) {
                return proc;
            }
            if (H_wglGetProcAddress.original()) {
                return H_wglGetProcAddress.original()(name);
            }
            return nullptr;
        }

        template<typename T>
        bool ensure_proc(T &slot, const char *name) {
            if (slot) return true;
            slot = reinterpret_cast<T>(resolve_gl_proc(name));
            return slot != nullptr;
        }

        std::string classify_shader_label(const ShaderRecord &record, std::string_view source) {
            if (contains(source, "ieeDiagFpspriteEasuDeltaMarker")) return "fpsprite-easu-delta";
            if (contains(source, "ieeDiagFpselectEasuDeltaMarker")) return "fpselect-easu-delta";
            if (contains(source, "ieeProbeFpspriteMarker")) return "fpsprite-probe";
            if (contains(source, "ieeProbeFpselectMarker")) return "fpselect-probe";
            if (contains(source, "ieeV1FpspriteMarker")) return "fpsprite-v1";
            if (contains(source, "ieeV1FpselectMarker")) return "fpselect-v1";
            if (contains(source, "IEE_DIAG_FPSPRITE_PROBE")) return "fpsprite-probe";
            if (contains(source, "IEE_DIAG_FPSELECT_PROBE")) return "fpselect-probe";
            if (contains(source, "IEE_V1_FPSPRITE")) return "fpsprite-v1";
            if (contains(source, "IEE_V1_FPSELECT")) return "fpselect-v1";
            if (record.type == GL_FRAGMENT_SHADER && record.hasSolidThreshold) return "fpselect-like";
            if (record.type == GL_FRAGMENT_SHADER && record.hasBlurAmount) return "fpsprite-like";
            if (record.type == GL_VERTEX_SHADER) return "vpdraw-like";
            if (record.type == GL_FRAGMENT_SHADER) return "fragment-other";
            return "other";
        }

        bool is_interesting_label(std::string_view label) {
            return contains(label, "sprite") || contains(label, "select") || contains(label, "seam");
        }

        bool is_tcscale_injectable_label(std::string_view label) {
            return contains(label, "fpsprite") || contains(label, "fpselect");
        }

        std::string join_shader_labels(const ProgramRecord &program) {
            std::string joined;
            bool first = true;
            for (const auto shaderId: program.attachedShaders) {
                const auto shaderIt = g_shaders.find(shaderId);
                if (shaderIt == g_shaders.end() || shaderIt->second.label.empty()) continue;
                if (!first) joined += ",";
                first = false;
                joined += shaderIt->second.label;
            }
            return joined;
        }

        struct UniformEntry {
            GLint location{-1};
            std::string name;
        };

        struct ProgramSnapshot {
            std::string label;
            bool interesting{};
            bool tcScaleInjectable{};
            GLint tcScaleLocation{-1};
            GLint texLocation{-1};
            GLint blurLocation{-1};
        };

        GLint uniform_location_by_name(const ProgramRecord &record, std::string_view name) {
            for (const auto &[location, uniformName]: record.uniformNames) {
                if (uniformName == name) return location;
            }
            return -1;
        }

        std::optional<ProgramSnapshot> snapshot_program(GLuint program) {
            std::lock_guard lock(g_mutex);
            const auto it = g_programs.find(program);
            if (it == g_programs.end()) return std::nullopt;

            ProgramSnapshot snapshot;
            snapshot.label = it->second.label;
            snapshot.interesting = it->second.interesting;
            snapshot.tcScaleInjectable = it->second.tcScaleInjectable;
            snapshot.tcScaleLocation = uniform_location_by_name(it->second, "uTcScale");
            snapshot.texLocation = uniform_location_by_name(it->second, "uTex");
            snapshot.blurLocation = uniform_location_by_name(it->second, "uSpriteBlurAmount");
            return snapshot;
        }

        std::vector<UniformEntry> snapshot_program_uniforms(GLuint program) {
            std::lock_guard lock(g_mutex);
            std::vector<UniformEntry> uniforms;

            const auto it = g_programs.find(program);
            if (it == g_programs.end()) return uniforms;

            uniforms.reserve(it->second.uniformNames.size());
            for (const auto &[location, name]: it->second.uniformNames) {
                uniforms.push_back(UniformEntry{location, name});
            }
            return uniforms;
        }

        bool update_last_uniform_value(GLuint program, GLint location, std::string_view value) {
            std::lock_guard lock(g_mutex);
            const auto it = g_programs.find(program);
            if (it == g_programs.end()) return false;
            auto &lastValue = it->second.lastUniformValues[location];
            if (lastValue == value) return false;
            lastValue = std::string(value);
            return true;
        }

        void log_injected_tcscale(GLuint program,
                                  GLint location,
                                  GLuint texture,
                                  GLint width,
                                  GLint height,
                                  std::string_view reason,
                                  std::string_view value) {
            std::string label;
            bool changed = false;

            {
                std::lock_guard lock(g_mutex);
                const auto it = g_programs.find(program);
                if (it == g_programs.end()) return;

                label = it->second.label;
                changed = it->second.lastInjectedTexture != texture ||
                          it->second.lastInjectedTcScaleValue != value;
                it->second.lastInjectedTexture = texture;
                it->second.lastInjectedTcScaleValue = std::string(value);
                it->second.lastUniformValues[location] = std::string(value);
            }

            if (!changed) return;

            LOG_INFO("ShaderTrace injectedTcScale program={} label={} reason={} texture={} size={}x{} uTcScale={}",
                     program,
                     label,
                     reason,
                     texture,
                     width,
                     height,
                     value);
        }

        std::optional<std::string> fetch_info_log(GLuint object, bool shaderLog) {
            constexpr GLsizei maxLength = 4096;

            if (shaderLog) {
                if (!ensure_proc(g_glGetShaderiv, "glGetShaderiv") ||
                    !ensure_proc(g_glGetShaderInfoLog, "glGetShaderInfoLog")) {
                    return std::nullopt;
                }
                GLint length = 0;
                g_glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
                if (length <= 1) return std::string{};
                std::string log(static_cast<std::size_t>(std::min(length, maxLength)), '\0');
                GLsizei written = 0;
                g_glGetShaderInfoLog(object, static_cast<GLsizei>(log.size()), &written, log.data());
                log.resize(static_cast<std::size_t>(std::max<GLsizei>(written, 0)));
                return log;
            }

            if (!ensure_proc(g_glGetProgramiv, "glGetProgramiv") ||
                !ensure_proc(g_glGetProgramInfoLog, "glGetProgramInfoLog")) {
                return std::nullopt;
            }
            GLint length = 0;
            g_glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
            if (length <= 1) return std::string{};
            std::string log(static_cast<std::size_t>(std::min(length, maxLength)), '\0');
            GLsizei written = 0;
            g_glGetProgramInfoLog(object, static_cast<GLsizei>(log.size()), &written, log.data());
            log.resize(static_cast<std::size_t>(std::max<GLsizei>(written, 0)));
            return log;
        }

        void query_interesting_uniforms(GLuint program) {
            if (!ensure_proc(g_glGetUniformLocation, "glGetUniformLocation")) {
                LOG_WARN("ShaderTrace unable to resolve glGetUniformLocation");
                return;
            }

            constexpr const char *uniforms[] = {
                "uTcScale",
                "uTex",
                "uSpriteBlurAmount",
            };

            for (const auto *name: uniforms) {
                const auto location = g_glGetUniformLocation(program, name);
                std::string label;
                {
                    std::lock_guard lock(g_mutex);
                    auto &record = g_programs[program];
                    label = record.label;
                    if (location >= 0) {
                        record.uniformNames[location] = name;
                    }
                }
                if (g_traceEnabled) {
                    LOG_INFO("ShaderTrace program={} label={} uniform={} location={}",
                             program,
                             label,
                             name,
                             location);
                }
            }
        }

        void log_uniform_update(GLint location, const std::string &value) {
            if (!g_traceEnabled) return;
            std::lock_guard lock(g_mutex);
            const auto programIt = g_programs.find(g_currentProgram);
            if (programIt == g_programs.end() || !programIt->second.interesting) return;

            auto &program = programIt->second;
            const auto uniformIt = program.uniformNames.find(location);
            if (uniformIt == program.uniformNames.end()) return;

            auto &lastValue = program.lastUniformValues[location];
            if (lastValue == value) return;
            lastValue = value;

            LOG_INFO("ShaderTrace uniform program={} label={} {}={}",
                     g_currentProgram,
                     program.label,
                     uniformIt->second,
                     value);
        }

        void log_current_uniform_values(GLuint program) {
            if (!g_traceEnabled) return;
            if (!ensure_proc(g_glGetUniformfv, "glGetUniformfv") ||
                !ensure_proc(g_glGetUniformiv, "glGetUniformiv")) {
                return;
            }

            const auto programSnapshot = snapshot_program(program);
            if (!programSnapshot || !programSnapshot->interesting) return;
            const auto uniforms = snapshot_program_uniforms(program);

            for (const auto &[location, name]: uniforms) {
                std::string value;

                if (name == "uTex") {
                    GLint texUnit = 0;
                    g_glGetUniformiv(program, location, &texUnit);
                    value = std::to_string(texUnit);
                } else if (name == "uTcScale") {
                    GLfloat tcScale[2] = {0.0f, 0.0f};
                    g_glGetUniformfv(program, location, tcScale);
                    value = std::to_string(tcScale[0]) + "," + std::to_string(tcScale[1]);
                } else if (name == "uSpriteBlurAmount") {
                    GLfloat blurAmount = 0.0f;
                    g_glGetUniformfv(program, location, &blurAmount);
                    value = std::to_string(blurAmount);
                } else {
                    continue;
                }

                if (!update_last_uniform_value(program, location, value)) continue;

                LOG_INFO("ShaderTrace uniformCurrent program={} label={} {}={}",
                         program,
                         programSnapshot->label,
                         name,
                         value);
            }
        }

        void maybe_inject_sprite_tcscale(GLuint program, std::string_view reason) {
            if (!g_tcScaleInjectionEnabled || program == 0) return;

            if (!ensure_proc(g_glGetUniformiv, "glGetUniformiv") ||
                !ensure_proc(g_glGetTexLevelParameteriv, "glGetTexLevelParameteriv")) {
                return;
            }
            if (!ensure_proc(g_glUniform2f, "glUniform2f") &&
                !ensure_proc(g_glUniform2fARB, "glUniform2fARB")) {
                return;
            }

            const auto snapshot = snapshot_program(program);
            if (!snapshot || !snapshot->tcScaleInjectable || snapshot->tcScaleLocation < 0 || snapshot->texLocation < 0) {
                return;
            }

            GLint samplerUnit = 0;
            g_glGetUniformiv(program, snapshot->texLocation, &samplerUnit);
            if (samplerUnit < 0) return;

            GLuint texture = 0;
            GLint previousActiveTextureUnit = 0;
            {
                std::lock_guard lock(g_mutex);
                previousActiveTextureUnit = g_currentActiveTextureUnit;
                const auto textureIt = g_boundTextures2D.find(samplerUnit);
                if (textureIt != g_boundTextures2D.end()) {
                    texture = textureIt->second;
                }
            }
            if (texture == 0) return;

            if (samplerUnit != previousActiveTextureUnit) {
                if (!ensure_proc(g_glActiveTexture, "glActiveTexture")) return;
                g_glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(samplerUnit));
            }

            GLint width = 0;
            GLint height = 0;
            g_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
            g_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

            if (samplerUnit != previousActiveTextureUnit) {
                g_glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(previousActiveTextureUnit));
            }

            if (width <= 0 || height <= 0) return;

            const auto tcScaleX = 1.0f / static_cast<float>(width);
            const auto tcScaleY = 1.0f / static_cast<float>(height);
            if (g_glUniform2f) {
                g_glUniform2f(snapshot->tcScaleLocation, tcScaleX, tcScaleY);
            } else {
                g_glUniform2fARB(snapshot->tcScaleLocation, tcScaleX, tcScaleY);
            }

            const auto value = std::to_string(tcScaleX) + "," + std::to_string(tcScaleY);
            log_injected_tcscale(program,
                                 snapshot->tcScaleLocation,
                                 texture,
                                 width,
                                 height,
                                 reason,
                                 value);
        }

        GLuint APIENTRY Detour_glCreateShader(GLenum type) {
            const auto shader = g_glCreateShader(type);
            std::lock_guard lock(g_mutex);
            g_shaders[shader].type = type;
            if (g_traceEnabled) {
                LOG_INFO("ShaderTrace glCreateShader shader={} type={}", shader, shader_type_name(type));
            }
            return shader;
        }

        void APIENTRY Detour_glDeleteShader(GLuint shader) {
            {
                std::lock_guard lock(g_mutex);
                g_shaders.erase(shader);
            }
            g_glDeleteShader(shader);
        }

        void APIENTRY Detour_glShaderSource(GLuint shader,
                                            GLsizei count,
                                            const GLchar *const *strings,
                                            const GLint *lengths) {
            std::string merged;
            for (GLsizei i = 0; i < count; ++i) {
                if (!strings || !strings[i]) continue;
                const auto length = lengths && lengths[i] >= 0
                                        ? static_cast<std::size_t>(lengths[i])
                                        : std::char_traits<char>::length(strings[i]);
                merged.append(strings[i], length);
            }

            {
                std::lock_guard lock(g_mutex);
                auto &record = g_shaders[shader];
                record.sourceHash = fnv1a64(merged);
                record.sourceLength = merged.size();
                record.hasTcScale = contains(merged, "uTcScale");
                record.hasBlurAmount = contains(merged, "uSpriteBlurAmount");
                record.hasSolidThreshold = contains(merged, "fSolidThreshold");
                record.hasEasu = contains(merged, "uhSampleEasu") || contains(merged, "FsrEasu");
                record.label = classify_shader_label(record, merged);

                if (g_traceEnabled) {
                    LOG_INFO("ShaderTrace glShaderSource shader={} type={} bytes={} hash=0x{:016X} label={} tcScale={} blurAmount={} solidThreshold={} easu={}",
                             shader,
                             shader_type_name(record.type),
                             record.sourceLength,
                             record.sourceHash,
                             record.label,
                             record.hasTcScale,
                             record.hasBlurAmount,
                             record.hasSolidThreshold,
                             record.hasEasu);

                    if (record.type == GL_FRAGMENT_SHADER &&
                        (record.hasTcScale || record.hasBlurAmount || record.hasSolidThreshold || record.hasEasu)) {
                        LOG_INFO("ShaderTrace glShaderSourceSnippet shader={} snippet={}",
                                 shader,
                                 collapse_whitespace(merged));
                    }
                }
            }

            g_glShaderSource(shader, count, strings, lengths);
        }

        void APIENTRY Detour_glCompileShader(GLuint shader) {
            g_glCompileShader(shader);

            if (!ensure_proc(g_glGetShaderiv, "glGetShaderiv")) return;
            GLint compileStatus = 0;
            g_glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
            const auto infoLog = fetch_info_log(shader, true).value_or(std::string{});

            std::lock_guard lock(g_mutex);
            auto &record = g_shaders[shader];
            record.compileSucceeded = compileStatus != 0;
            if (g_traceEnabled) {
                LOG_INFO("ShaderTrace glCompileShader shader={} label={} ok={} log={}",
                         shader,
                         record.label,
                         record.compileSucceeded,
                         infoLog.empty() ? "<empty>" : infoLog);
            }
        }

        GLuint APIENTRY Detour_glCreateProgram() {
            const auto program = g_glCreateProgram();
            std::lock_guard lock(g_mutex);
            g_programs.emplace(program, ProgramRecord{});
            if (g_traceEnabled) {
                LOG_INFO("ShaderTrace glCreateProgram program={}", program);
            }
            return program;
        }

        void APIENTRY Detour_glDeleteProgram(GLuint program) {
            {
                std::lock_guard lock(g_mutex);
                g_programs.erase(program);
            }
            g_glDeleteProgram(program);
        }

        void APIENTRY Detour_glAttachShader(GLuint program, GLuint shader) {
            g_glAttachShader(program, shader);

            std::lock_guard lock(g_mutex);
            auto &record = g_programs[program];
            if (std::find(record.attachedShaders.begin(), record.attachedShaders.end(), shader) == record.attachedShaders.end()) {
                record.attachedShaders.push_back(shader);
            }
            if (g_traceEnabled) {
                const auto shaderIt = g_shaders.find(shader);
                LOG_INFO("ShaderTrace glAttachShader program={} shader={} shaderLabel={}",
                         program,
                         shader,
                         shaderIt == g_shaders.end() ? "unknown" : shaderIt->second.label);
            }
        }

        void APIENTRY Detour_glLinkProgram(GLuint program) {
            g_glLinkProgram(program);

            if (!ensure_proc(g_glGetProgramiv, "glGetProgramiv")) return;
            GLint linkStatus = 0;
            g_glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
            const auto infoLog = fetch_info_log(program, false).value_or(std::string{});

            {
                std::lock_guard lock(g_mutex);
                auto &record = g_programs[program];
                record.linkSucceeded = linkStatus != 0;
                record.label = lower_copy(join_shader_labels(record));
                record.interesting = is_interesting_label(record.label);
                record.tcScaleInjectable = is_tcscale_injectable_label(record.label);
                if (g_traceEnabled) {
                    LOG_INFO("ShaderTrace glLinkProgram program={} label={} ok={} log={}",
                             program,
                             record.label,
                             record.linkSucceeded,
                             infoLog.empty() ? "<empty>" : infoLog);
                }
            }

            const auto snapshot = snapshot_program(program);
            if (snapshot && snapshot->interesting && linkStatus != 0) {
                query_interesting_uniforms(program);
            }
        }

        void APIENTRY Detour_glUseProgram(GLuint program) {
            g_glUseProgram(program);

            std::optional<ProgramSnapshot> snapshot;
            bool shouldTraceBind = false;
            {
                std::lock_guard lock(g_mutex);
                g_currentProgram = program;
                const auto it = g_programs.find(program);
                if (it != g_programs.end()) {
                    snapshot = ProgramSnapshot{
                        it->second.label,
                        it->second.interesting,
                        it->second.tcScaleInjectable,
                        uniform_location_by_name(it->second, "uTcScale"),
                        uniform_location_by_name(it->second, "uTex"),
                        uniform_location_by_name(it->second, "uSpriteBlurAmount"),
                    };
                }
                if (program != g_lastLoggedProgram) {
                    g_lastLoggedProgram = program;
                    shouldTraceBind = snapshot && snapshot->interesting;
                }
            }

            if (shouldTraceBind && g_traceEnabled && snapshot) {
                LOG_INFO("ShaderTrace glUseProgram program={} label={}", program, snapshot->label);
            }

            maybe_inject_sprite_tcscale(program, "use");
            if (shouldTraceBind) {
                log_current_uniform_values(program);
            }
        }

        void APIENTRY Detour_glUseProgramObjectARB(GLuint program) {
            g_glUseProgramObjectARB(program);

            std::optional<ProgramSnapshot> snapshot;
            bool shouldTraceBind = false;
            {
                std::lock_guard lock(g_mutex);
                g_currentProgram = program;
                const auto it = g_programs.find(program);
                if (it != g_programs.end()) {
                    snapshot = ProgramSnapshot{
                        it->second.label,
                        it->second.interesting,
                        it->second.tcScaleInjectable,
                        uniform_location_by_name(it->second, "uTcScale"),
                        uniform_location_by_name(it->second, "uTex"),
                        uniform_location_by_name(it->second, "uSpriteBlurAmount"),
                    };
                }
                if (program != g_lastLoggedProgram) {
                    g_lastLoggedProgram = program;
                    shouldTraceBind = snapshot && snapshot->interesting;
                }
            }

            if (shouldTraceBind && g_traceEnabled && snapshot) {
                LOG_INFO("ShaderTrace glUseProgramObjectARB program={} label={}", program, snapshot->label);
            }

            maybe_inject_sprite_tcscale(program, "use-arb");
            if (shouldTraceBind) {
                log_current_uniform_values(program);
            }
        }

        void APIENTRY Detour_glBindTexture(GLenum target, GLuint texture) {
            H_glBindTexture.original()(target, texture);
            if (target != GL_TEXTURE_2D) return;

            GLuint currentProgram = 0;
            {
                std::lock_guard lock(g_mutex);
                g_boundTextures2D[g_currentActiveTextureUnit] = texture;
                currentProgram = g_currentProgram;
            }

            maybe_inject_sprite_tcscale(currentProgram, "bind");
        }

        void APIENTRY Detour_glActiveTexture(GLenum texture) {
            g_glActiveTexture(texture);
            if (texture < GL_TEXTURE0) return;

            std::lock_guard lock(g_mutex);
            g_currentActiveTextureUnit = static_cast<GLint>(texture - GL_TEXTURE0);
        }

        void APIENTRY Detour_glUniform1i(GLint location, GLint v0) {
            g_glUniform1i(location, v0);
            log_uniform_update(location, std::to_string(v0));
        }

        void APIENTRY Detour_glUniform1f(GLint location, GLfloat v0) {
            g_glUniform1f(location, v0);
            log_uniform_update(location, std::to_string(v0));
        }

        void APIENTRY Detour_glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
            g_glUniform2f(location, v0, v1);
            log_uniform_update(location, std::to_string(v0) + "," + std::to_string(v1));
        }

        void APIENTRY Detour_glUniform2fv(GLint location, GLsizei count, const GLfloat *value) {
            g_glUniform2fv(location, count, value);
            if (!value || count <= 0) return;
            log_uniform_update(location, std::to_string(value[0]) + "," + std::to_string(value[1]));
        }

        void APIENTRY Detour_glUniform1iARB(GLint location, GLint v0) {
            g_glUniform1iARB(location, v0);
            log_uniform_update(location, std::to_string(v0));
        }

        void APIENTRY Detour_glUniform1fARB(GLint location, GLfloat v0) {
            g_glUniform1fARB(location, v0);
            log_uniform_update(location, std::to_string(v0));
        }

        void APIENTRY Detour_glUniform2fARB(GLint location, GLfloat v0, GLfloat v1) {
            g_glUniform2fARB(location, v0, v1);
            log_uniform_update(location, std::to_string(v0) + "," + std::to_string(v1));
        }

        void APIENTRY Detour_glUniform2fvARB(GLint location, GLsizei count, const GLfloat *value) {
            g_glUniform2fvARB(location, count, value);
            if (!value || count <= 0) return;
            log_uniform_update(location, std::to_string(value[0]) + "," + std::to_string(value[1]));
        }

        PROC WINAPI Detour_wglGetProcAddress(LPCSTR name) {
            const auto proc = H_wglGetProcAddress.original()(name);
            if (!name || !proc || !g_enabled) return proc;

            const std::string_view procName{name};
            if (procName == "glCreateShader") {
                g_glCreateShader = reinterpret_cast<Fn_glCreateShader>(proc);
                return reinterpret_cast<PROC>(&Detour_glCreateShader);
            }
            if (procName == "glDeleteShader") {
                g_glDeleteShader = reinterpret_cast<Fn_glDeleteShader>(proc);
                return reinterpret_cast<PROC>(&Detour_glDeleteShader);
            }
            if (procName == "glShaderSource") {
                g_glShaderSource = reinterpret_cast<Fn_glShaderSource>(proc);
                return reinterpret_cast<PROC>(&Detour_glShaderSource);
            }
            if (procName == "glCompileShader") {
                g_glCompileShader = reinterpret_cast<Fn_glCompileShader>(proc);
                return reinterpret_cast<PROC>(&Detour_glCompileShader);
            }
            if (procName == "glCreateProgram") {
                g_glCreateProgram = reinterpret_cast<Fn_glCreateProgram>(proc);
                return reinterpret_cast<PROC>(&Detour_glCreateProgram);
            }
            if (procName == "glDeleteProgram") {
                g_glDeleteProgram = reinterpret_cast<Fn_glDeleteProgram>(proc);
                return reinterpret_cast<PROC>(&Detour_glDeleteProgram);
            }
            if (procName == "glAttachShader") {
                g_glAttachShader = reinterpret_cast<Fn_glAttachShader>(proc);
                return reinterpret_cast<PROC>(&Detour_glAttachShader);
            }
            if (procName == "glLinkProgram") {
                g_glLinkProgram = reinterpret_cast<Fn_glLinkProgram>(proc);
                return reinterpret_cast<PROC>(&Detour_glLinkProgram);
            }
            if (procName == "glActiveTexture") {
                g_glActiveTexture = reinterpret_cast<Fn_glActiveTexture>(proc);
                return reinterpret_cast<PROC>(&Detour_glActiveTexture);
            }
            if (procName == "glUseProgram") {
                g_glUseProgram = reinterpret_cast<Fn_glUseProgram>(proc);
                return reinterpret_cast<PROC>(&Detour_glUseProgram);
            }
            if (procName == "glUseProgramObjectARB") {
                g_glUseProgramObjectARB = reinterpret_cast<Fn_glUseProgramObjectARB>(proc);
                return reinterpret_cast<PROC>(&Detour_glUseProgramObjectARB);
            }
            if (procName == "glUniform1i") {
                g_glUniform1i = reinterpret_cast<Fn_glUniform1i>(proc);
                return reinterpret_cast<PROC>(&Detour_glUniform1i);
            }
            if (procName == "glUniform1f") {
                g_glUniform1f = reinterpret_cast<Fn_glUniform1f>(proc);
                return reinterpret_cast<PROC>(&Detour_glUniform1f);
            }
            if (procName == "glUniform2f") {
                g_glUniform2f = reinterpret_cast<Fn_glUniform2f>(proc);
                return reinterpret_cast<PROC>(&Detour_glUniform2f);
            }
            if (procName == "glUniform2fv") {
                g_glUniform2fv = reinterpret_cast<Fn_glUniform2fv>(proc);
                return reinterpret_cast<PROC>(&Detour_glUniform2fv);
            }
            if (procName == "glUniform1iARB") {
                g_glUniform1iARB = reinterpret_cast<Fn_glUniform1iARB>(proc);
                return reinterpret_cast<PROC>(&Detour_glUniform1iARB);
            }
            if (procName == "glUniform1fARB") {
                g_glUniform1fARB = reinterpret_cast<Fn_glUniform1fARB>(proc);
                return reinterpret_cast<PROC>(&Detour_glUniform1fARB);
            }
            if (procName == "glUniform2fARB") {
                g_glUniform2fARB = reinterpret_cast<Fn_glUniform2fARB>(proc);
                return reinterpret_cast<PROC>(&Detour_glUniform2fARB);
            }
            if (procName == "glUniform2fvARB") {
                g_glUniform2fvARB = reinterpret_cast<Fn_glUniform2fvARB>(proc);
                return reinterpret_cast<PROC>(&Detour_glUniform2fvARB);
            }

            return proc;
        }
    }

    bool install(const core::EngineConfig &cfg) {
        g_traceEnabled = cfg.enableShaderTracing;
        g_tcScaleInjectionEnabled = cfg.enableSpriteTcScaleInjection;
        g_enabled = g_traceEnabled || g_tcScaleInjectionEnabled;
        if (!g_enabled) return true;
        if (g_installed) return true;

        static core::HookInit hookInit;

        auto *opengl32 = GetModuleHandleA("opengl32.dll");
        if (!opengl32) {
            opengl32 = LoadLibraryA("opengl32.dll");
        }
        if (!opengl32) {
            LOG_ERROR("ShaderTrace failed to load opengl32.dll");
            return false;
        }

        auto *target = reinterpret_cast<void *>(GetProcAddress(opengl32, "wglGetProcAddress"));
        if (!target) {
            LOG_ERROR("ShaderTrace failed to locate wglGetProcAddress");
            return false;
        }

        try {
            H_wglGetProcAddress.create(target, reinterpret_cast<void *>(&Detour_wglGetProcAddress));
            H_wglGetProcAddress.enable();

            if (g_tcScaleInjectionEnabled) {
                auto *bindTextureTarget = reinterpret_cast<void *>(GetProcAddress(opengl32, "glBindTexture"));
                if (!bindTextureTarget) {
                    LOG_ERROR("ShaderTrace failed to locate glBindTexture for sprite tc-scale injection");
                    H_wglGetProcAddress.disable();
                    return false;
                }

                H_glBindTexture.create(bindTextureTarget, reinterpret_cast<void *>(&Detour_glBindTexture));
                H_glBindTexture.enable();
            }

            g_installed = true;
            if (g_traceEnabled) {
                LOG_INFO("ShaderTrace installed on wglGetProcAddress");
            }
            if (g_tcScaleInjectionEnabled) {
                LOG_INFO("Sprite tc-scale injection enabled for fpsprite/fpselect programs");
            }
            return true;
        } catch (const std::exception &e) {
            H_glBindTexture.disable();
            H_wglGetProcAddress.disable();
            LOG_ERROR("ShaderTrace install failed: {}", e.what());
            return false;
        }
    }

    void uninstall() noexcept {
        if (!g_installed) return;
        H_glBindTexture.disable();
        H_wglGetProcAddress.disable();
        g_installed = false;
        g_enabled = false;
        g_traceEnabled = false;
        g_tcScaleInjectionEnabled = false;

        std::lock_guard lock(g_mutex);
        g_shaders.clear();
        g_programs.clear();
        g_boundTextures2D.clear();
        g_currentProgram = 0;
        g_lastLoggedProgram = 0;
        g_currentActiveTextureUnit = 0;
    }
}
