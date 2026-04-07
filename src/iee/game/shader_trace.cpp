#include "shader_trace.h"

#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "renderer.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
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
        constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
        constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
        constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
        constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
        constexpr GLenum GL_POINTS = 0x0000;
        constexpr GLenum GL_LINES = 0x0001;
        constexpr GLenum GL_LINE_LOOP = 0x0002;
        constexpr GLenum GL_LINE_STRIP = 0x0003;
        constexpr GLenum GL_TRIANGLES = 0x0004;
        constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
        constexpr GLenum GL_TRIANGLE_FAN = 0x0006;
        constexpr GLenum GL_QUADS = 0x0007;
        constexpr GLenum GL_QUAD_STRIP = 0x0008;
        constexpr GLenum GL_POLYGON = 0x0009;

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
        using Fn_glBindFramebuffer = void (APIENTRY*)(GLenum, GLuint);
        using Fn_glFramebufferTexture2D = void (APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
        using Fn_glDrawArrays = void (APIENTRY*)(GLenum, GLint, GLsizei);
        using Fn_glDrawElements = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void *);
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

        struct RuntimeSummaryKey {
            GLuint program{};
            GLuint framebuffer{};
            int engineTextureId{-1};

            bool operator==(const RuntimeSummaryKey &other) const noexcept {
                return program == other.program &&
                       framebuffer == other.framebuffer &&
                       engineTextureId == other.engineTextureId;
            }
        };

        struct RuntimeSummaryKeyHash {
            std::size_t operator()(const RuntimeSummaryKey &key) const noexcept {
                auto hash = static_cast<std::size_t>(key.program);
                hash ^= static_cast<std::size_t>(key.framebuffer) << 1;
                hash ^= static_cast<std::size_t>(static_cast<std::uint32_t>(key.engineTextureId)) << 2;
                return hash;
            }
        };

        struct RuntimeSummaryValue {
            std::string label;
            std::size_t drawArrays{};
            std::size_t drawElements{};
            std::size_t drawEnds{};
            GLint samplerUnit{-1};
            GLuint texture{};
            std::size_t lastLoggedTotal{};
        };

        struct GlTextureSummaryKey {
            GLuint program{};
            GLuint framebuffer{};
            GLint samplerUnit{-1};
            GLuint texture{};

            bool operator==(const GlTextureSummaryKey &other) const noexcept {
                return program == other.program &&
                       framebuffer == other.framebuffer &&
                       samplerUnit == other.samplerUnit &&
                       texture == other.texture;
            }
        };

        struct GlTextureSummaryKeyHash {
            std::size_t operator()(const GlTextureSummaryKey &key) const noexcept {
                auto hash = static_cast<std::size_t>(key.program);
                hash ^= static_cast<std::size_t>(key.framebuffer) << 1;
                hash ^= static_cast<std::size_t>(static_cast<std::uint32_t>(key.samplerUnit)) << 2;
                hash ^= static_cast<std::size_t>(key.texture) << 3;
                return hash;
            }
        };

        struct GlTextureSummaryValue {
            std::string label;
            std::size_t drawArrays{};
            std::size_t drawElements{};
            std::size_t lastLoggedTotal{};
        };

        struct BatchAggregateKey {
            GLuint program{};
            int engineTextureId{-1};
            int mode{};

            bool operator==(const BatchAggregateKey &other) const noexcept {
                return program == other.program &&
                       engineTextureId == other.engineTextureId &&
                       mode == other.mode;
            }
        };

        struct BatchAggregateKeyHash {
            std::size_t operator()(const BatchAggregateKey &key) const noexcept {
                auto hash = static_cast<std::size_t>(key.program);
                hash ^= static_cast<std::size_t>(static_cast<std::uint32_t>(key.engineTextureId)) << 1;
                hash ^= static_cast<std::size_t>(static_cast<std::uint32_t>(key.mode)) << 2;
                return hash;
            }
        };

        struct BatchCapture {
            bool active{};
            bool highlightApplied{};
            GLuint program{};
            std::string label;
            GLuint framebuffer{};
            int engineTextureId{-1};
            int mode{};
            unsigned long savedColor{};
            std::size_t texCoordCount{};
            std::size_t vertexCount{};
            bool hasVertexBounds{};
            bool hasUvBounds{};
            int minX{};
            int maxX{};
            int minY{};
            int maxY{};
            int minU{};
            int maxU{};
            int minV{};
            int maxV{};
        };

        struct BatchAggregateValue {
            std::string label;
            std::size_t batchCount{};
            std::size_t minVertexCount{std::numeric_limits<std::size_t>::max()};
            std::size_t maxVertexCount{};
            int minScreenWidth{std::numeric_limits<int>::max()};
            int maxScreenWidth{};
            int minScreenHeight{std::numeric_limits<int>::max()};
            int maxScreenHeight{};
            int minUvWidth{std::numeric_limits<int>::max()};
            int maxUvWidth{};
            int minUvHeight{std::numeric_limits<int>::max()};
            int maxUvHeight{};
            std::size_t lastLoggedBatchCount{};
        };

        core::Hook<Fn_glBindTexture> H_glBindTexture;
        core::Hook<Fn_glDrawArrays> H_glDrawArrays;
        core::Hook<Fn_glDrawElements> H_glDrawElements;
        core::Hook<Fn_wglGetProcAddress> H_wglGetProcAddress;
        core::Hook<DrawApi::DrawBegin_t> H_DrawBeginApi;
        core::Hook<DrawApi::DrawBindTexture_t> H_DrawBindTextureApi;
        core::Hook<DrawApi::DrawTexCoord_t> H_DrawTexCoordApi;
        core::Hook<DrawApi::DrawVertex_t> H_DrawVertexApi;
        core::Hook<DrawApi::DrawEnd_t> H_DrawEndApi;

        std::mutex g_mutex;
        std::unordered_map<GLuint, ShaderRecord> g_shaders;
        std::unordered_map<GLuint, ProgramRecord> g_programs;
        std::unordered_map<GLint, GLuint> g_boundTextures2D;
        std::unordered_map<RuntimeSummaryKey, RuntimeSummaryValue, RuntimeSummaryKeyHash> g_runtimeSummaries;
        std::unordered_map<GlTextureSummaryKey, GlTextureSummaryValue, GlTextureSummaryKeyHash> g_glTextureSummaries;
        std::unordered_map<BatchAggregateKey, BatchAggregateValue, BatchAggregateKeyHash> g_batchAggregates;
        GLuint g_currentProgram = 0;
        GLuint g_lastLoggedProgram = 0;
        GLuint g_currentFramebuffer = 0;
        GLint g_currentActiveTextureUnit = 0;
        std::size_t g_loggedDrawCallsForCurrentProgram = 0;
        bool g_suppressedDrawLogsForCurrentProgram = false;
        std::size_t g_loggedDrawEndsForCurrentProgram = 0;
        bool g_suppressedDrawEndLogsForCurrentProgram = false;
        int g_lastEngineTextureId = -1;
        bool g_traceEnabled = false;
        bool g_tcScaleInjectionEnabled = false;
        bool g_batchHighlightEnabled = false;
        bool g_batchSuppressEnabled = false;
        bool g_glProgramSuppressEnabled = false;
        bool g_glTextureSuppressEnabled = false;
        bool g_enabled = false;
        bool g_installed = false;
        bool g_drawApiAttached = false;
        BatchCapture g_currentBatch;
        GLuint g_batchHighlightProgram = 0;
        int g_batchHighlightTextureId = -1;
        int g_batchSuppressMinScreenWidth = 0;
        int g_batchSuppressMaxScreenWidth = 0;
        int g_batchSuppressMinScreenHeight = 0;
        int g_batchSuppressMaxScreenHeight = 0;
        int g_batchSuppressMinCenterX = -1;
        int g_batchSuppressMaxCenterX = -1;
        int g_batchSuppressMinCenterY = -1;
        int g_batchSuppressMaxCenterY = -1;
        GLuint g_glProgramSuppress = 0;
        GLuint g_glTextureSuppressProgram = 0;
        GLint g_glTextureSuppressTexture = -1;
        GLuint g_runtimeTraceProgramFilter = 0;
        GLint g_runtimeTraceTextureFilter = -1;
        DrawApi::DrawColor_t g_drawColorApi{};

        constexpr unsigned long kBatchHighlightColor = 0xFFFF00FF;

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
        Fn_glBindFramebuffer g_glBindFramebuffer{};
        Fn_glFramebufferTexture2D g_glFramebufferTexture2D{};
        Fn_glDrawArrays g_glDrawArrays{};
        Fn_glDrawElements g_glDrawElements{};
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

        const char *framebuffer_target_name(GLenum target) noexcept {
            switch (target) {
                case GL_FRAMEBUFFER: return "GL_FRAMEBUFFER";
                case GL_READ_FRAMEBUFFER: return "GL_READ_FRAMEBUFFER";
                case GL_DRAW_FRAMEBUFFER: return "GL_DRAW_FRAMEBUFFER";
                default: return "other";
            }
        }

        const char *draw_mode_name(GLenum mode) noexcept {
            switch (mode) {
                case GL_POINTS: return "GL_POINTS";
                case GL_LINES: return "GL_LINES";
                case GL_LINE_LOOP: return "GL_LINE_LOOP";
                case GL_LINE_STRIP: return "GL_LINE_STRIP";
                case GL_TRIANGLES: return "GL_TRIANGLES";
                case GL_TRIANGLE_STRIP: return "GL_TRIANGLE_STRIP";
                case GL_TRIANGLE_FAN: return "GL_TRIANGLE_FAN";
                case GL_QUADS: return "GL_QUADS";
                case GL_QUAD_STRIP: return "GL_QUAD_STRIP";
                case GL_POLYGON: return "GL_POLYGON";
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
            const auto lowerSource = lower_copy(source);

            if (contains(lowerSource, "ieediagfpdrawpathprobemarker")) return "fpdraw-path-probe";
            if (contains(lowerSource, "ieediagfpspriteeasudeltamarker")) return "fpsprite-easu-delta";
            if (contains(lowerSource, "ieediagfpselecteasudeltamarker")) return "fpselect-easu-delta";
            if (contains(lowerSource, "ieeprobefpspritemarker")) return "fpsprite-probe";
            if (contains(lowerSource, "ieeprobefpselectmarker")) return "fpselect-probe";
            if (contains(lowerSource, "ieev1fpspritemarker")) return "fpsprite-v1";
            if (contains(lowerSource, "ieev1fpselectmarker")) return "fpselect-v1";
            if (contains(lowerSource, "iee_v2_fpdraw_sprite_body")) return "fpdraw-v2-sprite-body";
            if (contains(lowerSource, "iee_diag_fpdraw_path_probe")) return "fpdraw-path-probe";
            if (contains(lowerSource, "iee_diag_fpsprite_probe")) return "fpsprite-probe";
            if (contains(lowerSource, "iee_diag_fpselect_probe")) return "fpselect-probe";
            if (contains(lowerSource, "iee_v1_fpsprite")) return "fpsprite-v1";
            if (contains(lowerSource, "iee_v1_fpselect")) return "fpselect-v1";
            if (contains(lowerSource, "fpdraw.glsl")) return "fpdraw-like";
            if (contains(lowerSource, "fptone.glsl")) return "fptone-like";
            if (contains(lowerSource, "fpyuvgry.glsl")) return "fpyuvgry-like";
            if (contains(lowerSource, "fpyuv.glsl")) return "fpyuv-like";
            if (contains(lowerSource, "fpfont.glsl")) return "fpfont-like";
            if (contains(lowerSource, "fpseam.glsl")) return "fpseam-like";
            if (contains(lowerSource, "fpsprite.glsl")) return "fpsprite-like";
            if (contains(lowerSource, "fpselect.glsl")) return "fpselect-like";
            if (record.type == GL_FRAGMENT_SHADER && record.hasSolidThreshold) return "fpselect-like";
            if (record.type == GL_FRAGMENT_SHADER && record.hasBlurAmount) return "fpsprite-like";
            if (record.type == GL_VERTEX_SHADER) return "vpdraw-like";
            if (record.type == GL_FRAGMENT_SHADER) return "fragment-other";
            return "other";
        }

        bool is_interesting_label(std::string_view label) {
            return contains(label, "sprite") || contains(label, "select") || contains(label, "seam") || contains(
                       label, "fpdraw");
        }

        bool is_tcscale_injectable_label(std::string_view label) {
            return contains(label, "fpsprite") || contains(label, "fpselect");
        }

        bool runtime_trace_filter_matches(GLuint program, GLuint texture) noexcept {
            if (g_runtimeTraceProgramFilter > 0 && program != g_runtimeTraceProgramFilter) {
                return false;
            }
            if (g_runtimeTraceTextureFilter >= 0 &&
                static_cast<GLint>(texture) != g_runtimeTraceTextureFilter) {
                return false;
            }
            return true;
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
            GLint spriteBodyModeLocation{-1};
        };

        struct DrawCallSnapshot {
            GLuint program{};
            std::string label;
            bool interesting{};
            GLuint framebuffer{};
            int engineTextureId{-1};
            GLint samplerUnit{-1};
            GLuint texture{};
            std::size_t loggedDrawCalls{};
            bool suppressedDrawLogs{};
        };

        bool is_sprite_body_draw(const DrawCallSnapshot &snapshot) noexcept {
            return snapshot.program == 3 && snapshot.texture == 2;
        }

        struct EngineBatchSnapshot {
            GLuint program{};
            std::string label;
            bool interesting{};
            GLuint framebuffer{};
            int engineTextureId{-1};
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
            snapshot.spriteBodyModeLocation = uniform_location_by_name(it->second, "uIeeSpriteBodyMode");
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

        std::optional<DrawCallSnapshot> snapshot_current_draw_call() {
            std::lock_guard lock(g_mutex);
            DrawCallSnapshot snapshot;
            snapshot.program = g_currentProgram;
            snapshot.framebuffer = g_currentFramebuffer;
            snapshot.engineTextureId = g_lastEngineTextureId;
            snapshot.loggedDrawCalls = g_loggedDrawCallsForCurrentProgram;
            snapshot.suppressedDrawLogs = g_suppressedDrawLogsForCurrentProgram;

            const auto programIt = g_programs.find(g_currentProgram);
            if (programIt != g_programs.end()) {
                snapshot.label = programIt->second.label;
                snapshot.interesting = programIt->second.interesting;

                for (const auto &[location, name]: programIt->second.uniformNames) {
                    if (name != "uTex") continue;
                    if (!ensure_proc(g_glGetUniformiv, "glGetUniformiv")) break;
                    GLint samplerUnit = 0;
                    g_glGetUniformiv(g_currentProgram, location, &samplerUnit);
                    snapshot.samplerUnit = samplerUnit;
                    const auto textureIt = g_boundTextures2D.find(samplerUnit);
                    if (textureIt != g_boundTextures2D.end()) {
                        snapshot.texture = textureIt->second;
                    }
                    break;
                }
            } else {
                snapshot.label = g_currentProgram == 0 ? "program-0" : "untracked";
                snapshot.interesting = false;
            }

            return snapshot;
        }

        bool should_emit_runtime_summary(std::size_t total, std::size_t lastLoggedTotal) noexcept {
            if (total <= lastLoggedTotal) return false;
            return total == 1 || total == 4 || total == 16 || (total % 64) == 0;
        }

        void reset_batch_capture(BatchCapture &capture) noexcept {
            capture = BatchCapture{};
        }

        void update_int_bounds(bool &hasBounds, int value, int &minValue, int &maxValue) noexcept {
            if (!hasBounds) {
                hasBounds = true;
                minValue = value;
                maxValue = value;
                return;
            }

            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }

        void update_2d_bounds(bool &hasBounds,
                              int x,
                              int y,
                              int &minX,
                              int &maxX,
                              int &minY,
                              int &maxY) noexcept {
            if (!hasBounds) {
                hasBounds = true;
                minX = maxX = x;
                minY = maxY = y;
                return;
            }

            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }

        void maybe_highlight_current_batch() {
            if (!g_batchHighlightEnabled || !g_drawColorApi) return;

            std::string label;
            int engineTextureId = -1;
            GLuint program = 0;
            unsigned long savedColor = 0;

            {
                std::lock_guard lock(g_mutex);
                if (!g_currentBatch.active || g_currentBatch.highlightApplied) return;
                if (g_currentBatch.program != g_batchHighlightProgram) return;
                if (g_currentBatch.engineTextureId != g_batchHighlightTextureId) return;

                label = g_currentBatch.label;
                engineTextureId = g_currentBatch.engineTextureId;
                program = g_currentBatch.program;
            }

            savedColor = g_drawColorApi(kBatchHighlightColor);

            {
                std::lock_guard lock(g_mutex);
                if (!g_currentBatch.active || g_currentBatch.highlightApplied) {
                    g_drawColorApi(savedColor);
                    return;
                }
                if (g_currentBatch.program != program || g_currentBatch.engineTextureId != engineTextureId) {
                    g_drawColorApi(savedColor);
                    return;
                }

                g_currentBatch.highlightApplied = true;
                g_currentBatch.savedColor = savedColor;
            }

            LOG_INFO("ShaderTrace highlightedBatch program={} label={} engineTexId={} color=0x{:08X}",
                     program,
                     label,
                     engineTextureId,
                     kBatchHighlightColor);
        }

        bool should_suppress_batch(const BatchCapture &capture) noexcept {
            if (!g_batchSuppressEnabled) return false;
            if (!capture.active || capture.program != g_batchHighlightProgram) return false;
            if (g_batchHighlightTextureId >= 0 && capture.engineTextureId != g_batchHighlightTextureId) return false;
            if (!capture.hasVertexBounds) return false;

            const auto screenWidth = capture.maxX - capture.minX;
            const auto screenHeight = capture.maxY - capture.minY;
            const auto centerX = (capture.minX + capture.maxX) / 2;
            const auto centerY = (capture.minY + capture.maxY) / 2;

            if (g_batchSuppressMinScreenWidth > 0 && screenWidth < g_batchSuppressMinScreenWidth) return false;
            if (g_batchSuppressMaxScreenWidth > 0 && screenWidth > g_batchSuppressMaxScreenWidth) return false;
            if (g_batchSuppressMinScreenHeight > 0 && screenHeight < g_batchSuppressMinScreenHeight) return false;
            if (g_batchSuppressMaxScreenHeight > 0 && screenHeight > g_batchSuppressMaxScreenHeight) return false;
            if (g_batchSuppressMinCenterX >= 0 && centerX < g_batchSuppressMinCenterX) return false;
            if (g_batchSuppressMaxCenterX >= 0 && centerX > g_batchSuppressMaxCenterX) return false;
            if (g_batchSuppressMinCenterY >= 0 && centerY < g_batchSuppressMinCenterY) return false;
            if (g_batchSuppressMaxCenterY >= 0 && centerY > g_batchSuppressMaxCenterY) return false;

            return true;
        }

        void record_runtime_summary(const DrawCallSnapshot &snapshot, bool drawElements) {
            if (!g_traceEnabled) return;
            if (snapshot.program == 0) return;
            if (!runtime_trace_filter_matches(snapshot.program, snapshot.texture)) return;

            RuntimeSummaryValue value;
            {
                std::lock_guard lock(g_mutex);
                auto &summary = g_runtimeSummaries[RuntimeSummaryKey{
                    snapshot.program,
                    snapshot.framebuffer,
                    snapshot.engineTextureId,
                }];

                if (summary.label.empty()) summary.label = snapshot.label;
                summary.samplerUnit = snapshot.samplerUnit;
                summary.texture = snapshot.texture;
                if (drawElements) {
                    ++summary.drawElements;
                } else {
                    ++summary.drawArrays;
                }

                const auto total = summary.drawArrays + summary.drawElements + summary.drawEnds;
                if (!should_emit_runtime_summary(total, summary.lastLoggedTotal)) return;
                summary.lastLoggedTotal = total;
                value = summary;
            }

            LOG_INFO(
                "ShaderTrace runtimeSummary program={} label={} framebuffer={} engineTexId={} drawArrays={} drawElements={} drawEnds={} samplerUnit={} texture={}",
                snapshot.program,
                snapshot.label,
                snapshot.framebuffer,
                snapshot.engineTextureId,
                value.drawArrays,
                value.drawElements,
                value.drawEnds,
                value.samplerUnit,
                value.texture);

            if (snapshot.texture == 0 || snapshot.samplerUnit < 0) return;

            GlTextureSummaryValue glValue;
            {
                std::lock_guard lock(g_mutex);
                auto &summary = g_glTextureSummaries[GlTextureSummaryKey{
                    snapshot.program,
                    snapshot.framebuffer,
                    snapshot.samplerUnit,
                    snapshot.texture,
                }];

                if (summary.label.empty()) summary.label = snapshot.label;
                if (drawElements) {
                    ++summary.drawElements;
                } else {
                    ++summary.drawArrays;
                }

                const auto total = summary.drawArrays + summary.drawElements;
                if (!should_emit_runtime_summary(total, summary.lastLoggedTotal)) return;
                summary.lastLoggedTotal = total;
                glValue = summary;
            }

            LOG_INFO(
                "ShaderTrace glTextureSummary program={} label={} framebuffer={} samplerUnit={} texture={} drawArrays={} drawElements={}",
                snapshot.program,
                snapshot.label,
                snapshot.framebuffer,
                snapshot.samplerUnit,
                snapshot.texture,
                glValue.drawArrays,
                glValue.drawElements);
        }

        bool should_suppress_gl_draw(const DrawCallSnapshot &snapshot) noexcept {
            if (g_glProgramSuppressEnabled && snapshot.program == g_glProgramSuppress) {
                return true;
            }

            return g_glTextureSuppressEnabled &&
                   snapshot.program == g_glTextureSuppressProgram &&
                   snapshot.texture != 0 &&
                   static_cast<GLint>(snapshot.texture) == g_glTextureSuppressTexture;
        }

        void record_runtime_summary(const EngineBatchSnapshot &snapshot) {
            if (!g_traceEnabled) return;
            if (snapshot.program == 0) return;
            if (!runtime_trace_filter_matches(snapshot.program, 0)) return;

            RuntimeSummaryValue value;
            {
                std::lock_guard lock(g_mutex);
                auto &summary = g_runtimeSummaries[RuntimeSummaryKey{
                    snapshot.program,
                    snapshot.framebuffer,
                    snapshot.engineTextureId,
                }];

                if (summary.label.empty()) summary.label = snapshot.label;
                ++summary.drawEnds;

                const auto total = summary.drawArrays + summary.drawElements + summary.drawEnds;
                if (!should_emit_runtime_summary(total, summary.lastLoggedTotal)) return;
                summary.lastLoggedTotal = total;
                value = summary;
            }

            LOG_INFO(
                "ShaderTrace runtimeSummary program={} label={} framebuffer={} engineTexId={} drawArrays={} drawElements={} drawEnds={} samplerUnit={} texture={}",
                snapshot.program,
                snapshot.label,
                snapshot.framebuffer,
                snapshot.engineTextureId,
                value.drawArrays,
                value.drawElements,
                value.drawEnds,
                value.samplerUnit,
                value.texture);
        }

        void record_batch_aggregate(const BatchCapture &capture) {
            if (!g_traceEnabled || !capture.active) return;
            if (capture.program == 0 || capture.engineTextureId < 0) return;
            if (!runtime_trace_filter_matches(capture.program, 0)) return;

            const auto screenWidth = capture.hasVertexBounds ? capture.maxX - capture.minX : 0;
            const auto screenHeight = capture.hasVertexBounds ? capture.maxY - capture.minY : 0;
            const auto uvWidth = capture.hasUvBounds ? capture.maxU - capture.minU : 0;
            const auto uvHeight = capture.hasUvBounds ? capture.maxV - capture.minV : 0;

            BatchAggregateValue value;
            {
                std::lock_guard lock(g_mutex);
                auto &aggregate = g_batchAggregates[BatchAggregateKey{
                    capture.program,
                    capture.engineTextureId,
                    capture.mode,
                }];

                if (aggregate.label.empty()) aggregate.label = capture.label;
                ++aggregate.batchCount;
                aggregate.minVertexCount = std::min(aggregate.minVertexCount, capture.vertexCount);
                aggregate.maxVertexCount = std::max(aggregate.maxVertexCount, capture.vertexCount);
                aggregate.minScreenWidth = std::min(aggregate.minScreenWidth, screenWidth);
                aggregate.maxScreenWidth = std::max(aggregate.maxScreenWidth, screenWidth);
                aggregate.minScreenHeight = std::min(aggregate.minScreenHeight, screenHeight);
                aggregate.maxScreenHeight = std::max(aggregate.maxScreenHeight, screenHeight);
                aggregate.minUvWidth = std::min(aggregate.minUvWidth, uvWidth);
                aggregate.maxUvWidth = std::max(aggregate.maxUvWidth, uvWidth);
                aggregate.minUvHeight = std::min(aggregate.minUvHeight, uvHeight);
                aggregate.maxUvHeight = std::max(aggregate.maxUvHeight, uvHeight);

                if (!should_emit_runtime_summary(aggregate.batchCount, aggregate.lastLoggedBatchCount)) return;
                aggregate.lastLoggedBatchCount = aggregate.batchCount;
                value = aggregate;
            }

            LOG_INFO(
                "ShaderTrace batchSummary program={} label={} framebuffer={} engineTexId={} mode={} batches={} verts[min={},max={}] screen[min={}x{},max={}x{}] uv[min={}x{},max={}x{}] sampleVerts={} sampleScreen={}x{} sampleUv={}x{}",
                capture.program,
                capture.label,
                capture.framebuffer,
                capture.engineTextureId,
                draw_mode_name(static_cast<GLenum>(capture.mode)),
                value.batchCount,
                value.minVertexCount == std::numeric_limits<std::size_t>::max() ? 0 : value.minVertexCount,
                value.maxVertexCount,
                value.minScreenWidth == std::numeric_limits<int>::max() ? 0 : value.minScreenWidth,
                value.minScreenHeight == std::numeric_limits<int>::max() ? 0 : value.minScreenHeight,
                value.maxScreenWidth,
                value.maxScreenHeight,
                value.minUvWidth == std::numeric_limits<int>::max() ? 0 : value.minUvWidth,
                value.minUvHeight == std::numeric_limits<int>::max() ? 0 : value.minUvHeight,
                value.maxUvWidth,
                value.maxUvHeight,
                capture.vertexCount,
                screenWidth,
                screenHeight,
                uvWidth,
                uvHeight);
        }

        std::optional<EngineBatchSnapshot> snapshot_current_engine_batch() {
            std::lock_guard lock(g_mutex);
            EngineBatchSnapshot snapshot;
            snapshot.program = g_currentProgram;
            snapshot.framebuffer = g_currentFramebuffer;
            snapshot.engineTextureId = g_lastEngineTextureId;
            const auto programIt = g_programs.find(g_currentProgram);
            if (programIt != g_programs.end()) {
                snapshot.label = programIt->second.label;
                snapshot.interesting = programIt->second.interesting;
            } else {
                snapshot.label = g_currentProgram == 0 ? "program-0" : "untracked";
                snapshot.interesting = false;
            }
            return snapshot;
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

        void maybe_set_sprite_body_mode(const DrawCallSnapshot &snapshot) {
            if (!ensure_proc(g_glUniform1f, "glUniform1f") &&
                !ensure_proc(g_glUniform1fARB, "glUniform1fARB")) {
                return;
            }

            const auto programSnapshot = snapshot_program(snapshot.program);
            if (!programSnapshot || programSnapshot->spriteBodyModeLocation < 0) return;

            const auto mode = is_sprite_body_draw(snapshot) ? 1.0f : 0.0f;
            const auto value = std::to_string(mode);
            if (!update_last_uniform_value(snapshot.program, programSnapshot->spriteBodyModeLocation, value)) return;

            if (g_glUniform1f) {
                g_glUniform1f(programSnapshot->spriteBodyModeLocation, mode);
            } else {
                g_glUniform1fARB(programSnapshot->spriteBodyModeLocation, mode);
            }

            if (g_traceEnabled && runtime_trace_filter_matches(snapshot.program, snapshot.texture)) {
                LOG_INFO("ShaderTrace spriteBodyMode program={} label={} texture={} value={}",
                         snapshot.program,
                         programSnapshot->label,
                         snapshot.texture,
                         value);
            }
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
                "uIeeSpriteBodyMode",
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
            if (g_runtimeTraceProgramFilter > 0 || g_runtimeTraceTextureFilter >= 0) {
                const auto drawSnapshot = snapshot_current_draw_call();
                if (!drawSnapshot ||
                    !runtime_trace_filter_matches(drawSnapshot->program, drawSnapshot->texture)) {
                    return;
                }
            }
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
                } else if (name == "uIeeSpriteBodyMode") {
                    GLfloat spriteBodyMode = 0.0f;
                    g_glGetUniformfv(program, location, &spriteBodyMode);
                    value = std::to_string(spriteBodyMode);
                } else {
                    continue;
                }

                if (!update_last_uniform_value(program, location, value)) continue;

                if ((g_runtimeTraceProgramFilter > 0 || g_runtimeTraceTextureFilter >= 0) &&
                    !runtime_trace_filter_matches(program, 0)) {
                    continue;
                }

                LOG_INFO("ShaderTrace uniformCurrent program={} label={} {}={}",
                         program,
                         programSnapshot->label,
                         name,
                         value);
            }
        }

        bool begin_draw_log(std::size_t *outDrawIndex = nullptr, bool *outWasSuppressed = nullptr) {
            std::lock_guard lock(g_mutex);
            if (outDrawIndex) *outDrawIndex = g_loggedDrawCallsForCurrentProgram;
            if (outWasSuppressed) *outWasSuppressed = g_suppressedDrawLogsForCurrentProgram;

            constexpr std::size_t maxLogsPerProgramBind = 3;
            if (g_loggedDrawCallsForCurrentProgram < maxLogsPerProgramBind) {
                ++g_loggedDrawCallsForCurrentProgram;
                return true;
            }

            if (!g_suppressedDrawLogsForCurrentProgram) {
                g_suppressedDrawLogsForCurrentProgram = true;
                return true;
            }

            return false;
        }

        bool begin_draw_end_log(std::size_t *outDrawIndex = nullptr, bool *outWasSuppressed = nullptr) {
            std::lock_guard lock(g_mutex);
            if (outDrawIndex) *outDrawIndex = g_loggedDrawEndsForCurrentProgram;
            if (outWasSuppressed) *outWasSuppressed = g_suppressedDrawEndLogsForCurrentProgram;

            constexpr std::size_t maxLogsPerProgramBind = 4;
            if (g_loggedDrawEndsForCurrentProgram < maxLogsPerProgramBind) {
                ++g_loggedDrawEndsForCurrentProgram;
                return true;
            }

            if (!g_suppressedDrawEndLogsForCurrentProgram) {
                g_suppressedDrawEndLogsForCurrentProgram = true;
                return true;
            }

            return false;
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
                        uniform_location_by_name(it->second, "uIeeSpriteBodyMode"),
                    };
                }
                if (program != g_lastLoggedProgram) {
                    g_lastLoggedProgram = program;
                    g_loggedDrawCallsForCurrentProgram = 0;
                    g_suppressedDrawLogsForCurrentProgram = false;
                    g_loggedDrawEndsForCurrentProgram = 0;
                    g_suppressedDrawEndLogsForCurrentProgram = false;
                    shouldTraceBind = true;
                }
            }

            if (shouldTraceBind && g_traceEnabled) {
                LOG_INFO("ShaderTrace glUseProgram program={} label={}",
                         program,
                         snapshot ? snapshot->label : (program == 0 ? "program-0" : "untracked"));
            }

            maybe_inject_sprite_tcscale(program, "use");
            if (shouldTraceBind && snapshot && snapshot->interesting) {
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
                        uniform_location_by_name(it->second, "uIeeSpriteBodyMode"),
                    };
                }
                if (program != g_lastLoggedProgram) {
                    g_lastLoggedProgram = program;
                    g_loggedDrawCallsForCurrentProgram = 0;
                    g_suppressedDrawLogsForCurrentProgram = false;
                    g_loggedDrawEndsForCurrentProgram = 0;
                    g_suppressedDrawEndLogsForCurrentProgram = false;
                    shouldTraceBind = true;
                }
            }

            if (shouldTraceBind && g_traceEnabled) {
                LOG_INFO("ShaderTrace glUseProgramObjectARB program={} label={}",
                         program,
                         snapshot ? snapshot->label : (program == 0 ? "program-0" : "untracked"));
            }

            maybe_inject_sprite_tcscale(program, "use-arb");
            if (shouldTraceBind && snapshot && snapshot->interesting) {
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

        void Detour_DrawBindTexture(int textureId) {
            H_DrawBindTextureApi.original()(textureId);

            {
                std::lock_guard lock(g_mutex);
                g_lastEngineTextureId = textureId;
                if (g_currentBatch.active) {
                    g_currentBatch.engineTextureId = textureId;
                }
            }

            maybe_highlight_current_batch();
        }

        void Detour_DrawBegin(int mode) {
            H_DrawBeginApi.original()(mode);

            if (!g_traceEnabled && !g_batchHighlightEnabled && !g_batchSuppressEnabled) return;

            bool shouldHighlight = false;
            {
                std::lock_guard lock(g_mutex);
                reset_batch_capture(g_currentBatch);
                g_currentBatch.active = true;
                g_currentBatch.program = g_currentProgram;
                g_currentBatch.framebuffer = g_currentFramebuffer;
                g_currentBatch.engineTextureId = g_lastEngineTextureId;
                g_currentBatch.mode = mode;

                const auto it = g_programs.find(g_currentProgram);
                g_currentBatch.label = it != g_programs.end()
                                           ? it->second.label
                                           : (g_currentProgram == 0 ? "program-0" : "untracked");

                shouldHighlight = g_batchHighlightEnabled &&
                                  g_drawColorApi &&
                                  g_currentProgram == g_batchHighlightProgram &&
                                  g_lastEngineTextureId == g_batchHighlightTextureId;
            }

            if (shouldHighlight) {
                maybe_highlight_current_batch();
            }
        }

        void Detour_DrawTexCoord(int u, int v) {
            H_DrawTexCoordApi.original()(u, v);

            if (!g_traceEnabled) return;

            std::lock_guard lock(g_mutex);
            if (!g_currentBatch.active) return;
            ++g_currentBatch.texCoordCount;
            update_2d_bounds(g_currentBatch.hasUvBounds,
                             u,
                             v,
                             g_currentBatch.minU,
                             g_currentBatch.maxU,
                             g_currentBatch.minV,
                             g_currentBatch.maxV);
        }

        void Detour_DrawVertex(int x, int y) {
            H_DrawVertexApi.original()(x, y);

            if (!g_traceEnabled && !g_batchSuppressEnabled) return;

            std::lock_guard lock(g_mutex);
            if (!g_currentBatch.active) return;
            ++g_currentBatch.vertexCount;
            update_2d_bounds(g_currentBatch.hasVertexBounds,
                             x,
                             y,
                             g_currentBatch.minX,
                             g_currentBatch.maxX,
                             g_currentBatch.minY,
                             g_currentBatch.maxY);
        }

        void Detour_DrawEnd() {
            BatchCapture capture;
            {
                std::lock_guard lock(g_mutex);
                capture = g_currentBatch;
                reset_batch_capture(g_currentBatch);
            }

            const auto suppressBatch = should_suppress_batch(capture);
            if (!suppressBatch) {
                H_DrawEndApi.original()();
            }

            if (capture.highlightApplied && g_drawColorApi) {
                g_drawColorApi(capture.savedColor);
            }

            if (suppressBatch) {
                const auto screenWidth = capture.hasVertexBounds ? capture.maxX - capture.minX : 0;
                const auto screenHeight = capture.hasVertexBounds ? capture.maxY - capture.minY : 0;
                LOG_INFO("ShaderTrace suppressedBatch program={} label={} framebuffer={} engineTexId={} screen={}x{} verts={}",
                         capture.program,
                         capture.label,
                         capture.framebuffer,
                         capture.engineTextureId,
                         screenWidth,
                         screenHeight,
                         capture.vertexCount);
            }

            if (!g_traceEnabled) return;

            record_batch_aggregate(capture);

            const auto snapshot = snapshot_current_engine_batch();
            if (!snapshot) return;
            record_runtime_summary(*snapshot);

            std::size_t drawIndex = 0;
            bool wasSuppressed = false;
            if (!begin_draw_end_log(&drawIndex, &wasSuppressed)) return;

            if (wasSuppressed) {
                LOG_INFO("ShaderTrace DrawEnd logs suppressed for current bind program={} label={}",
                         snapshot->program,
                         snapshot->label);
                return;
            }

            LOG_INFO("ShaderTrace DrawEnd program={} label={} batch={} framebuffer={} engineTexId={}",
                     snapshot->program,
                     snapshot->label,
                     drawIndex + 1,
                     snapshot->framebuffer,
                     snapshot->engineTextureId);
        }

        void APIENTRY Detour_glActiveTexture(GLenum texture) {
            g_glActiveTexture(texture);
            if (texture < GL_TEXTURE0) return;

            std::lock_guard lock(g_mutex);
            g_currentActiveTextureUnit = static_cast<GLint>(texture - GL_TEXTURE0);
        }

        void APIENTRY Detour_glBindFramebuffer(GLenum target, GLuint framebuffer) {
            g_glBindFramebuffer(target, framebuffer);

            bool changed = false;
            GLuint program = 0;
            std::string label;
            {
                std::lock_guard lock(g_mutex);
                if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
                    changed = g_currentFramebuffer != framebuffer;
                    g_currentFramebuffer = framebuffer;
                }

                program = g_currentProgram;
                const auto it = g_programs.find(program);
                if (it != g_programs.end()) {
                    label = it->second.label;
                }
            }

            if (!g_traceEnabled || !changed) return;

            LOG_INFO("ShaderTrace glBindFramebuffer target={} framebuffer={} currentProgram={} label={}",
                     framebuffer_target_name(target),
                     framebuffer,
                     program,
                     label.empty() ? "unknown" : label);
        }

        void APIENTRY Detour_glFramebufferTexture2D(GLenum target,
                                                    GLenum attachment,
                                                    GLenum textarget,
                                                    GLuint texture,
                                                    GLint level) {
            g_glFramebufferTexture2D(target, attachment, textarget, texture, level);

            if (!g_traceEnabled) return;
            if (attachment != GL_COLOR_ATTACHMENT0) return;

            GLuint framebuffer = 0;
            {
                std::lock_guard lock(g_mutex);
                framebuffer = g_currentFramebuffer;
            }

            LOG_INFO("ShaderTrace glFramebufferTexture2D target={} framebuffer={} attachment=0x{:X} textarget=0x{:X} texture={} level={}",
                     framebuffer_target_name(target),
                     framebuffer,
                     attachment,
                     textarget,
                     texture,
                     level);
        }

        void APIENTRY Detour_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
            const auto snapshot = snapshot_current_draw_call();
            if (!snapshot) return;

            maybe_set_sprite_body_mode(*snapshot);

            if (should_suppress_gl_draw(*snapshot)) {
                LOG_INFO("ShaderTrace suppressedGlDrawArrays program={} label={} mode={} framebuffer={} first={} count={} samplerUnit={} texture={}",
                         snapshot->program,
                         snapshot->label,
                         draw_mode_name(mode),
                         snapshot->framebuffer,
                         first,
                         count,
                         snapshot->samplerUnit,
                         snapshot->texture);
                return;
            }

            H_glDrawArrays.original()(mode, first, count);

            if (!g_traceEnabled) return;
            record_runtime_summary(*snapshot, false);
            if (!runtime_trace_filter_matches(snapshot->program, snapshot->texture)) return;

            std::size_t drawIndex = 0;
            bool wasSuppressed = false;
            if (!begin_draw_log(&drawIndex, &wasSuppressed)) return;

            if (wasSuppressed) {
                LOG_INFO("ShaderTrace draw logs suppressed for current bind program={} label={}",
                         snapshot->program,
                         snapshot->label);
                return;
            }

            LOG_INFO("ShaderTrace glDrawArrays program={} label={} draw={} mode={} framebuffer={} first={} count={} samplerUnit={} texture={}",
                     snapshot->program,
                     snapshot->label,
                     drawIndex + 1,
                     draw_mode_name(mode),
                     snapshot->framebuffer,
                     first,
                     count,
                     snapshot->samplerUnit,
                     snapshot->texture);
        }

        void APIENTRY Detour_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
            const auto snapshot = snapshot_current_draw_call();
            if (!snapshot) return;

            maybe_set_sprite_body_mode(*snapshot);

            if (should_suppress_gl_draw(*snapshot)) {
                LOG_INFO("ShaderTrace suppressedGlDrawElements program={} label={} mode={} framebuffer={} count={} type=0x{:X} samplerUnit={} texture={}",
                         snapshot->program,
                         snapshot->label,
                         draw_mode_name(mode),
                         snapshot->framebuffer,
                         count,
                         type,
                         snapshot->samplerUnit,
                         snapshot->texture);
                return;
            }

            H_glDrawElements.original()(mode, count, type, indices);

            if (!g_traceEnabled) return;
            record_runtime_summary(*snapshot, true);
            if (!runtime_trace_filter_matches(snapshot->program, snapshot->texture)) return;

            std::size_t drawIndex = 0;
            bool wasSuppressed = false;
            if (!begin_draw_log(&drawIndex, &wasSuppressed)) return;

            if (wasSuppressed) {
                LOG_INFO("ShaderTrace draw logs suppressed for current bind program={} label={}",
                         snapshot->program,
                         snapshot->label);
                return;
            }

            LOG_INFO("ShaderTrace glDrawElements program={} label={} draw={} mode={} framebuffer={} count={} type=0x{:X} samplerUnit={} texture={}",
                     snapshot->program,
                     snapshot->label,
                     drawIndex + 1,
                     draw_mode_name(mode),
                     snapshot->framebuffer,
                     count,
                     type,
                     snapshot->samplerUnit,
                     snapshot->texture);
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
            if (procName == "glBindFramebuffer" || procName == "glBindFramebufferEXT") {
                g_glBindFramebuffer = reinterpret_cast<Fn_glBindFramebuffer>(proc);
                return reinterpret_cast<PROC>(&Detour_glBindFramebuffer);
            }
            if (procName == "glFramebufferTexture2D" || procName == "glFramebufferTexture2DEXT") {
                g_glFramebufferTexture2D = reinterpret_cast<Fn_glFramebufferTexture2D>(proc);
                return reinterpret_cast<PROC>(&Detour_glFramebufferTexture2D);
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
        g_batchHighlightEnabled = cfg.enableBatchHighlightProbe &&
                                  cfg.batchHighlightProgram > 0 &&
                                  cfg.batchHighlightTextureId >= 0;
        g_batchSuppressEnabled = cfg.enableBatchSuppressProbe &&
                                 cfg.batchHighlightProgram > 0;
        g_glProgramSuppressEnabled = cfg.enableGlProgramSuppressProbe && cfg.glProgramSuppress > 0;
        g_glTextureSuppressEnabled = cfg.enableGlTextureSuppressProbe &&
                                     cfg.glTextureSuppressProgram > 0 &&
                                     cfg.glTextureSuppressTexture > 0;
        g_batchHighlightProgram = static_cast<GLuint>(std::max(cfg.batchHighlightProgram, 0));
        g_batchHighlightTextureId = cfg.batchHighlightTextureId;
        g_batchSuppressMinScreenWidth = std::max(cfg.batchSuppressMinScreenWidth, 0);
        g_batchSuppressMaxScreenWidth = std::max(cfg.batchSuppressMaxScreenWidth, 0);
        g_batchSuppressMinScreenHeight = std::max(cfg.batchSuppressMinScreenHeight, 0);
        g_batchSuppressMaxScreenHeight = std::max(cfg.batchSuppressMaxScreenHeight, 0);
        g_batchSuppressMinCenterX = cfg.batchSuppressMinCenterX;
        g_batchSuppressMaxCenterX = cfg.batchSuppressMaxCenterX;
        g_batchSuppressMinCenterY = cfg.batchSuppressMinCenterY;
        g_batchSuppressMaxCenterY = cfg.batchSuppressMaxCenterY;
        g_glProgramSuppress = static_cast<GLuint>(std::max(cfg.glProgramSuppress, 0));
        g_glTextureSuppressProgram = static_cast<GLuint>(std::max(cfg.glTextureSuppressProgram, 0));
        g_glTextureSuppressTexture = cfg.glTextureSuppressTexture;
        g_runtimeTraceProgramFilter = static_cast<GLuint>(std::max(cfg.shaderTraceRuntimeProgramFilter, 0));
        g_runtimeTraceTextureFilter = cfg.shaderTraceRuntimeTextureFilter;
        g_enabled = g_traceEnabled || g_tcScaleInjectionEnabled || g_batchHighlightEnabled || g_batchSuppressEnabled ||
                    g_glProgramSuppressEnabled || g_glTextureSuppressEnabled;
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

            if (g_tcScaleInjectionEnabled || g_traceEnabled) {
                auto *bindTextureTarget = reinterpret_cast<void *>(GetProcAddress(opengl32, "glBindTexture"));
                if (!bindTextureTarget) {
                    LOG_ERROR("ShaderTrace failed to locate glBindTexture for runtime sprite tracing");
                    H_wglGetProcAddress.disable();
                    return false;
                }

                H_glBindTexture.create(bindTextureTarget, reinterpret_cast<void *>(&Detour_glBindTexture));
                H_glBindTexture.enable();
            }

            if (g_traceEnabled || g_glProgramSuppressEnabled || g_glTextureSuppressEnabled) {
                auto *drawArraysTarget = reinterpret_cast<void *>(GetProcAddress(opengl32, "glDrawArrays"));
                auto *drawElementsTarget = reinterpret_cast<void *>(GetProcAddress(opengl32, "glDrawElements"));
                if (!drawArraysTarget || !drawElementsTarget) {
                    LOG_ERROR("ShaderTrace failed to locate glDrawArrays/glDrawElements");
                    H_glBindTexture.disable();
                    H_wglGetProcAddress.disable();
                    return false;
                }

                H_glDrawArrays.create(drawArraysTarget, reinterpret_cast<void *>(&Detour_glDrawArrays));
                H_glDrawArrays.enable();
                H_glDrawElements.create(drawElementsTarget, reinterpret_cast<void *>(&Detour_glDrawElements));
                H_glDrawElements.enable();
            }

            g_installed = true;
            if (g_traceEnabled) {
                LOG_INFO("ShaderTrace installed on wglGetProcAddress");
            }
            if (g_tcScaleInjectionEnabled) {
                LOG_INFO("Sprite tc-scale injection enabled for fpsprite/fpselect programs");
            }
            if (g_batchHighlightEnabled) {
                LOG_INFO("Batch highlight probe enabled for program={} engineTexId={} color=0x{:08X}",
                         g_batchHighlightProgram,
                         g_batchHighlightTextureId,
                         kBatchHighlightColor);
            } else if (cfg.enableBatchHighlightProbe) {
                LOG_WARN("Batch highlight probe enabled in config but inactive because program={} textureId={}",
                         cfg.batchHighlightProgram,
                         cfg.batchHighlightTextureId);
            }
            if (g_batchSuppressEnabled) {
                LOG_INFO("Batch suppress probe enabled for program={} engineTexId={} screen={}..{}x{}..{} center={}..{}x{}..{}",
                         g_batchHighlightProgram,
                         g_batchHighlightTextureId,
                         g_batchSuppressMinScreenWidth,
                         g_batchSuppressMaxScreenWidth,
                         g_batchSuppressMinScreenHeight,
                         g_batchSuppressMaxScreenHeight,
                         g_batchSuppressMinCenterX,
                         g_batchSuppressMaxCenterX,
                         g_batchSuppressMinCenterY,
                         g_batchSuppressMaxCenterY);
            } else if (cfg.enableBatchSuppressProbe) {
                LOG_WARN("Batch suppress probe enabled in config but inactive because program={}",
                         cfg.batchHighlightProgram);
            }
            if (g_glProgramSuppressEnabled) {
                LOG_INFO("GL program suppress probe enabled for program={}", g_glProgramSuppress);
            } else if (cfg.enableGlProgramSuppressProbe) {
                LOG_WARN("GL program suppress probe enabled in config but inactive because program={}",
                         cfg.glProgramSuppress);
            }
            if (g_glTextureSuppressEnabled) {
                LOG_INFO("GL texture suppress probe enabled for program={} texture={}",
                         g_glTextureSuppressProgram,
                         g_glTextureSuppressTexture);
            } else if (cfg.enableGlTextureSuppressProbe) {
                LOG_WARN("GL texture suppress probe enabled in config but inactive because program={} texture={}",
                         cfg.glTextureSuppressProgram,
                         cfg.glTextureSuppressTexture);
            }
            if (g_traceEnabled && (g_runtimeTraceProgramFilter > 0 || g_runtimeTraceTextureFilter >= 0)) {
                LOG_INFO("ShaderTrace runtime log filter enabled for program={} texture={}",
                         g_runtimeTraceProgramFilter,
                         g_runtimeTraceTextureFilter);
            }
            return true;
        } catch (const std::exception &e) {
            H_glDrawElements.disable();
            H_glDrawArrays.disable();
            H_glBindTexture.disable();
            H_wglGetProcAddress.disable();
            LOG_ERROR("ShaderTrace install failed: {}", e.what());
            return false;
        }
    }

    bool attach_draw_api(const DrawApi &draw) {
        if ((!g_traceEnabled && !g_batchHighlightEnabled && !g_batchSuppressEnabled) || g_drawApiAttached) return true;
        if (!draw.DrawBegin || !draw.DrawBindTexture || !draw.DrawTexCoord || !draw.DrawVertex || !draw.DrawEnd) {
            LOG_WARN("ShaderTrace draw API attach skipped: required draw API functions are null");
            return false;
        }

        static core::HookInit hookInit;

        try {
            g_drawColorApi = draw.DrawColor;
            H_DrawBeginApi.create(reinterpret_cast<void *>(draw.DrawBegin),
                                  reinterpret_cast<void *>(&Detour_DrawBegin));
            H_DrawBeginApi.enable();

            H_DrawBindTextureApi.create(reinterpret_cast<void *>(draw.DrawBindTexture),
                                        reinterpret_cast<void *>(&Detour_DrawBindTexture));
            H_DrawBindTextureApi.enable();

            H_DrawTexCoordApi.create(reinterpret_cast<void *>(draw.DrawTexCoord),
                                     reinterpret_cast<void *>(&Detour_DrawTexCoord));
            H_DrawTexCoordApi.enable();

            H_DrawVertexApi.create(reinterpret_cast<void *>(draw.DrawVertex),
                                   reinterpret_cast<void *>(&Detour_DrawVertex));
            H_DrawVertexApi.enable();

            H_DrawEndApi.create(reinterpret_cast<void *>(draw.DrawEnd),
                                reinterpret_cast<void *>(&Detour_DrawEnd));
            H_DrawEndApi.enable();

            g_drawApiAttached = true;
            if (g_batchHighlightEnabled && !g_drawColorApi) {
                LOG_WARN("Batch highlight probe requested but DrawColor is unavailable; tint override will be inactive");
            }
            LOG_INFO("ShaderTrace attached to DrawBegin/DrawBindTexture/DrawTexCoord/DrawVertex/DrawEnd");
            return true;
        } catch (const std::exception &e) {
            H_DrawEndApi.disable();
            H_DrawVertexApi.disable();
            H_DrawTexCoordApi.disable();
            H_DrawBindTextureApi.disable();
            H_DrawBeginApi.disable();
            LOG_ERROR("ShaderTrace draw API attach failed: {}", e.what());
            return false;
        }
    }

    void reset_runtime_capture(const char *reason) noexcept {
        if (!g_enabled) return;

        {
            std::lock_guard lock(g_mutex);
            g_runtimeSummaries.clear();
            g_glTextureSummaries.clear();
            g_batchAggregates.clear();
            g_loggedDrawCallsForCurrentProgram = 0;
            g_suppressedDrawLogsForCurrentProgram = false;
            g_loggedDrawEndsForCurrentProgram = 0;
            g_suppressedDrawEndLogsForCurrentProgram = false;
            g_lastEngineTextureId = -1;
            reset_batch_capture(g_currentBatch);
        }

        if (g_traceEnabled) {
            LOG_INFO("ShaderTrace runtime capture reset reason={}", reason ? reason : "unspecified");
        }
    }

    void uninstall() noexcept {
        if (!g_installed) return;
        H_DrawEndApi.disable();
        H_DrawVertexApi.disable();
        H_DrawTexCoordApi.disable();
        H_DrawBindTextureApi.disable();
        H_DrawBeginApi.disable();
        H_glDrawElements.disable();
        H_glDrawArrays.disable();
        H_glBindTexture.disable();
        H_wglGetProcAddress.disable();
        g_installed = false;
        g_enabled = false;
        g_traceEnabled = false;
        g_tcScaleInjectionEnabled = false;
        g_batchHighlightEnabled = false;
        g_batchSuppressEnabled = false;
        g_glProgramSuppressEnabled = false;
        g_glTextureSuppressEnabled = false;
        g_drawApiAttached = false;
        g_batchHighlightProgram = 0;
        g_batchHighlightTextureId = -1;
        g_batchSuppressMinScreenWidth = 0;
        g_batchSuppressMaxScreenWidth = 0;
        g_batchSuppressMinScreenHeight = 0;
        g_batchSuppressMaxScreenHeight = 0;
        g_batchSuppressMinCenterX = -1;
        g_batchSuppressMaxCenterX = -1;
        g_batchSuppressMinCenterY = -1;
        g_batchSuppressMaxCenterY = -1;
        g_glProgramSuppress = 0;
        g_glTextureSuppressProgram = 0;
        g_glTextureSuppressTexture = -1;
        g_runtimeTraceProgramFilter = 0;
        g_runtimeTraceTextureFilter = -1;
        g_drawColorApi = nullptr;

        std::lock_guard lock(g_mutex);
        g_shaders.clear();
        g_programs.clear();
        g_boundTextures2D.clear();
        g_runtimeSummaries.clear();
        g_glTextureSummaries.clear();
        g_batchAggregates.clear();
        g_currentProgram = 0;
        g_lastLoggedProgram = 0;
        g_currentFramebuffer = 0;
        g_currentActiveTextureUnit = 0;
        g_loggedDrawCallsForCurrentProgram = 0;
        g_suppressedDrawLogsForCurrentProgram = false;
        g_loggedDrawEndsForCurrentProgram = 0;
        g_suppressedDrawEndLogsForCurrentProgram = false;
        g_lastEngineTextureId = -1;
        reset_batch_capture(g_currentBatch);
    }
}
