#include "sprite_body_fsr.h"

#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace iee::game::sprite_body_fsr {
    namespace {
        using GLfloat = float;
        using GLchar = char;
        using GLboolean = unsigned char;
        using Fn_SwapBuffers = BOOL (WINAPI*)(HDC);

        constexpr GLenum GL_FALSE = 0;
        constexpr GLenum GL_TRUE = 1;
        constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
        constexpr GLenum GL_TEXTURE0 = 0x84C0;
        constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
        constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
        constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
        constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
        constexpr GLenum GL_LINEAR = 0x2601;
        constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
        constexpr GLenum GL_RGBA = 0x1908;
        constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
        constexpr GLenum GL_COLOR_BUFFER_BIT = 0x00004000;
        constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
        constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
        constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
        constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
        constexpr GLenum GL_VIEWPORT = 0x0BA2;
        constexpr GLenum GL_SCISSOR_BOX = 0x0C10;
        constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
        constexpr GLenum GL_BLEND = 0x0BE2;
        constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;
        constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;
        constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
        constexpr GLenum GL_BLEND_SRC_RGB = 0x80C9;
        constexpr GLenum GL_BLEND_DST_RGB = 0x80C8;
        constexpr GLenum GL_BLEND_SRC_ALPHA = 0x80CB;
        constexpr GLenum GL_BLEND_DST_ALPHA = 0x80CA;
        constexpr GLenum GL_BLEND_EQUATION_RGB = 0x8009;
        constexpr GLenum GL_BLEND_EQUATION_ALPHA = 0x883D;
        constexpr GLenum GL_FUNC_ADD = 0x8006;
        constexpr GLenum GL_ONE = 1;
        constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
        constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
        constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
        constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
        constexpr GLenum GL_LINK_STATUS = 0x8B82;
        constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
        constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;

        using Fn_glGetIntegerv = void (APIENTRY*)(GLenum, GLint *);
        using Fn_glIsEnabled = GLboolean (APIENTRY*)(GLenum);
        using Fn_glEnable = void (APIENTRY*)(GLenum);
        using Fn_glDisable = void (APIENTRY*)(GLenum);
        using Fn_glViewport = void (APIENTRY*)(GLint, GLint, GLsizei, GLsizei);
        using Fn_glScissor = void (APIENTRY*)(GLint, GLint, GLsizei, GLsizei);
        using Fn_glClearColor = void (APIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
        using Fn_glClear = void (APIENTRY*)(GLenum);
        using Fn_glBlendFunc = void (APIENTRY*)(GLenum, GLenum);
        using Fn_glBlendEquation = void (APIENTRY*)(GLenum);
        using Fn_glActiveTexture = void (APIENTRY*)(GLenum);
        using Fn_glBindTexture = void (APIENTRY*)(GLenum, GLuint);
        using Fn_glGenTextures = void (APIENTRY*)(GLsizei, GLuint *);
        using Fn_glDeleteTextures = void (APIENTRY*)(GLsizei, const GLuint *);
        using Fn_glTexParameteri = void (APIENTRY*)(GLenum, GLenum, GLint);
        using Fn_glTexImage2D = void (APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                                 const void *);
        using Fn_glGenFramebuffers = void (APIENTRY*)(GLsizei, GLuint *);
        using Fn_glDeleteFramebuffers = void (APIENTRY*)(GLsizei, const GLuint *);
        using Fn_glBindFramebuffer = void (APIENTRY*)(GLenum, GLuint);
        using Fn_glFramebufferTexture2D = void (APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
        using Fn_glCheckFramebufferStatus = GLenum (APIENTRY*)(GLenum);
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
        using Fn_glUseProgram = void (APIENTRY*)(GLuint);
        using Fn_glGetUniformLocation = GLint (APIENTRY*)(GLuint, const GLchar *);
        using Fn_glUniform1i = void (APIENTRY*)(GLint, GLint);
        using Fn_glUniform1f = void (APIENTRY*)(GLint, GLfloat);
        using Fn_glUniform2f = void (APIENTRY*)(GLint, GLfloat, GLfloat);
        using Fn_glBegin = void (APIENTRY*)(GLenum);
        using Fn_glEnd = void (APIENTRY*)();
        using Fn_glTexCoord2f = void (APIENTRY*)(GLfloat, GLfloat);
        using Fn_glVertex2f = void (APIENTRY*)(GLfloat, GLfloat);

        core::Hook<Fn_SwapBuffers> H_SwapBuffers;

        thread_local bool g_internalPassActive = false;

        bool g_enabled = false;
        bool g_installed = false;
        GLuint g_targetProgram = 3;
        GLuint g_targetTexture = 2;
        float g_inputScale = 0.667f;
        bool g_enableRcas = true;
        float g_rcasSharpness = 0.20f;
        int g_debugView = 0;

        GLuint g_spriteInputFbo = 0;
        GLuint g_spriteInputTex = 0;
        GLuint g_spriteUpscaleFbo = 0;
        GLuint g_spriteUpscaleTex = 0;
        int g_inputWidth = 0;
        int g_inputHeight = 0;
        int g_displayWidth = 0;
        int g_displayHeight = 0;

        bool g_capturePending = false;
        GLuint g_displayFramebuffer = 0;
        std::array<GLint, 4> g_displayViewport{};
        std::array<GLint, 4> g_displayScissor{};
        bool g_displayScissorEnabled = false;
        bool g_displayBlendEnabled = false;
        GLint g_displayBlendSrcRgb = GL_ONE;
        GLint g_displayBlendDstRgb = GL_ONE_MINUS_SRC_ALPHA;
        GLint g_displayBlendSrcAlpha = GL_ONE;
        GLint g_displayBlendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
        GLint g_displayBlendEquationRgb = GL_FUNC_ADD;
        GLint g_displayBlendEquationAlpha = GL_FUNC_ADD;

        struct PassProgram {
            GLuint id = 0;
            GLint uTex = -1;
            GLint uInputSize = -1;
            GLint uOutputSize = -1;
            GLint uSharpness = -1;
        };

        PassProgram g_blitProgram;
        PassProgram g_easuProgram;
        PassProgram g_rcasProgram;
        bool g_programsReady = false;

        Fn_glGetIntegerv g_glGetIntegerv{};
        Fn_glIsEnabled g_glIsEnabled{};
        Fn_glEnable g_glEnable{};
        Fn_glDisable g_glDisable{};
        Fn_glViewport g_glViewport{};
        Fn_glScissor g_glScissor{};
        Fn_glClearColor g_glClearColor{};
        Fn_glClear g_glClear{};
        Fn_glBlendFunc g_glBlendFunc{};
        Fn_glBlendEquation g_glBlendEquation{};
        Fn_glActiveTexture g_glActiveTexture{};
        Fn_glBindTexture g_glBindTexture{};
        Fn_glGenTextures g_glGenTextures{};
        Fn_glDeleteTextures g_glDeleteTextures{};
        Fn_glTexParameteri g_glTexParameteri{};
        Fn_glTexImage2D g_glTexImage2D{};
        Fn_glGenFramebuffers g_glGenFramebuffers{};
        Fn_glDeleteFramebuffers g_glDeleteFramebuffers{};
        Fn_glBindFramebuffer g_glBindFramebuffer{};
        Fn_glFramebufferTexture2D g_glFramebufferTexture2D{};
        Fn_glCheckFramebufferStatus g_glCheckFramebufferStatus{};
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
        Fn_glUseProgram g_glUseProgram{};
        Fn_glGetUniformLocation g_glGetUniformLocation{};
        Fn_glUniform1i g_glUniform1i{};
        Fn_glUniform1f g_glUniform1f{};
        Fn_glUniform2f g_glUniform2f{};
        Fn_glBegin g_glBegin{};
        Fn_glEnd g_glEnd{};
        Fn_glTexCoord2f g_glTexCoord2f{};
        Fn_glVertex2f g_glVertex2f{};

        struct ScopedInternalPass {
            ScopedInternalPass() noexcept { g_internalPassActive = true; }

            ~ScopedInternalPass() { g_internalPassActive = false; }
        };

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

            using Fn_wglGetProcAddress = PROC (WINAPI*)(LPCSTR);
            static auto wglGetProcAddress = reinterpret_cast<Fn_wglGetProcAddress>(
                GetProcAddress(opengl32, "wglGetProcAddress"));
            return wglGetProcAddress ? wglGetProcAddress(name) : nullptr;
        }

        template<typename T>
        bool ensure_proc(T &slot, const char *name, const char *fallback = nullptr) {
            if (slot) return true;
            slot = reinterpret_cast<T>(resolve_gl_proc(name));
            if (!slot && fallback) {
                slot = reinterpret_cast<T>(resolve_gl_proc(fallback));
            }
            return slot != nullptr;
        }

        bool ensure_gl_runtime() {
            return ensure_proc(g_glGetIntegerv, "glGetIntegerv") &&
                   ensure_proc(g_glIsEnabled, "glIsEnabled") &&
                   ensure_proc(g_glEnable, "glEnable") &&
                   ensure_proc(g_glDisable, "glDisable") &&
                   ensure_proc(g_glViewport, "glViewport") &&
                   ensure_proc(g_glScissor, "glScissor") &&
                   ensure_proc(g_glClearColor, "glClearColor") &&
                   ensure_proc(g_glClear, "glClear") &&
                   ensure_proc(g_glBlendFunc, "glBlendFunc") &&
                   ensure_proc(g_glBlendEquation, "glBlendEquation", "glBlendEquationEXT") &&
                   ensure_proc(g_glActiveTexture, "glActiveTexture", "glActiveTextureARB") &&
                   ensure_proc(g_glBindTexture, "glBindTexture") &&
                   ensure_proc(g_glGenTextures, "glGenTextures") &&
                   ensure_proc(g_glDeleteTextures, "glDeleteTextures") &&
                   ensure_proc(g_glTexParameteri, "glTexParameteri") &&
                   ensure_proc(g_glTexImage2D, "glTexImage2D") &&
                   ensure_proc(g_glGenFramebuffers, "glGenFramebuffers", "glGenFramebuffersEXT") &&
                   ensure_proc(g_glDeleteFramebuffers, "glDeleteFramebuffers", "glDeleteFramebuffersEXT") &&
                   ensure_proc(g_glBindFramebuffer, "glBindFramebuffer", "glBindFramebufferEXT") &&
                   ensure_proc(g_glFramebufferTexture2D, "glFramebufferTexture2D", "glFramebufferTexture2DEXT") &&
                   ensure_proc(g_glCheckFramebufferStatus, "glCheckFramebufferStatus", "glCheckFramebufferStatusEXT") &&
                   ensure_proc(g_glCreateShader, "glCreateShader") &&
                   ensure_proc(g_glDeleteShader, "glDeleteShader") &&
                   ensure_proc(g_glShaderSource, "glShaderSource") &&
                   ensure_proc(g_glCompileShader, "glCompileShader") &&
                   ensure_proc(g_glGetShaderiv, "glGetShaderiv") &&
                   ensure_proc(g_glGetShaderInfoLog, "glGetShaderInfoLog") &&
                   ensure_proc(g_glCreateProgram, "glCreateProgram") &&
                   ensure_proc(g_glDeleteProgram, "glDeleteProgram") &&
                   ensure_proc(g_glAttachShader, "glAttachShader") &&
                   ensure_proc(g_glLinkProgram, "glLinkProgram") &&
                   ensure_proc(g_glGetProgramiv, "glGetProgramiv") &&
                   ensure_proc(g_glGetProgramInfoLog, "glGetProgramInfoLog") &&
                   ensure_proc(g_glUseProgram, "glUseProgram") &&
                   ensure_proc(g_glGetUniformLocation, "glGetUniformLocation") &&
                   ensure_proc(g_glUniform1i, "glUniform1i") &&
                   ensure_proc(g_glUniform1f, "glUniform1f") &&
                   ensure_proc(g_glUniform2f, "glUniform2f") &&
                   ensure_proc(g_glBegin, "glBegin") &&
                   ensure_proc(g_glEnd, "glEnd") &&
                   ensure_proc(g_glTexCoord2f, "glTexCoord2f") &&
                   ensure_proc(g_glVertex2f, "glVertex2f");
        }

        GLint clamp_positive(GLint value) noexcept {
            return std::max<GLint>(value, 1);
        }

        GLint scaled_dimension(GLint displaySize, float scale) noexcept {
            return clamp_positive(static_cast<GLint>(std::lround(static_cast<double>(displaySize) * scale)));
        }

        GLint scale_coord(GLint value, GLint displaySize, GLint targetSize) noexcept {
            if (displaySize <= 0 || targetSize <= 0) return value;
            return static_cast<GLint>(std::lround(static_cast<double>(value) * targetSize / displaySize));
        }

        void scale_rect(const std::array<GLint, 4> &displayRect,
                        GLint displayWidth,
                        GLint displayHeight,
                        GLint targetWidth,
                        GLint targetHeight,
                        std::array<GLint, 4> &outRect) noexcept {
            outRect[0] = scale_coord(displayRect[0], displayWidth, targetWidth);
            outRect[1] = scale_coord(displayRect[1], displayHeight, targetHeight);
            outRect[2] = clamp_positive(scale_coord(displayRect[2], displayWidth, targetWidth));
            outRect[3] = clamp_positive(scale_coord(displayRect[3], displayHeight, targetHeight));
        }

        void query_display_state() {
            g_glGetIntegerv(GL_VIEWPORT, g_displayViewport.data());
            g_glGetIntegerv(GL_SCISSOR_BOX, g_displayScissor.data());
            g_displayScissorEnabled = g_glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
            g_displayBlendEnabled = g_glIsEnabled(GL_BLEND) == GL_TRUE;
            g_glGetIntegerv(GL_BLEND_SRC_RGB, &g_displayBlendSrcRgb);
            g_glGetIntegerv(GL_BLEND_DST_RGB, &g_displayBlendDstRgb);
            g_glGetIntegerv(GL_BLEND_SRC_ALPHA, &g_displayBlendSrcAlpha);
            g_glGetIntegerv(GL_BLEND_DST_ALPHA, &g_displayBlendDstAlpha);
            g_glGetIntegerv(GL_BLEND_EQUATION_RGB, &g_displayBlendEquationRgb);
            g_glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &g_displayBlendEquationAlpha);
            g_displayWidth = clamp_positive(g_displayViewport[2]);
            g_displayHeight = clamp_positive(g_displayViewport[3]);
            g_displayFramebuffer = 0;
        }

        void destroy_pass_program(PassProgram &program) noexcept {
            if (program.id != 0 && g_glDeleteProgram) {
                ScopedInternalPass guard;
                g_glDeleteProgram(program.id);
            }
            program = PassProgram{};
        }

        void destroy_resources() noexcept {
            if (!ensure_gl_runtime()) return;

            ScopedInternalPass guard;
            if (g_spriteUpscaleFbo != 0) {
                g_glDeleteFramebuffers(1, &g_spriteUpscaleFbo);
                g_spriteUpscaleFbo = 0;
            }
            if (g_spriteInputFbo != 0) {
                g_glDeleteFramebuffers(1, &g_spriteInputFbo);
                g_spriteInputFbo = 0;
            }
            if (g_spriteUpscaleTex != 0) {
                g_glDeleteTextures(1, &g_spriteUpscaleTex);
                g_spriteUpscaleTex = 0;
            }
            if (g_spriteInputTex != 0) {
                g_glDeleteTextures(1, &g_spriteInputTex);
                g_spriteInputTex = 0;
            }
            g_inputWidth = 0;
            g_inputHeight = 0;
            g_displayWidth = 0;
            g_displayHeight = 0;
        }

        std::string shader_info_log(GLuint shader) {
            GLint length = 0;
            g_glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
            if (length <= 1) return {};
            std::string log(static_cast<std::size_t>(length), '\0');
            GLsizei written = 0;
            g_glGetShaderInfoLog(shader, length, &written, log.data());
            if (written > 0 && static_cast<std::size_t>(written) < log.size()) {
                log.resize(static_cast<std::size_t>(written));
            }
            return log;
        }

        std::string program_info_log(GLuint program) {
            GLint length = 0;
            g_glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            if (length <= 1) return {};
            std::string log(static_cast<std::size_t>(length), '\0');
            GLsizei written = 0;
            g_glGetProgramInfoLog(program, length, &written, log.data());
            if (written > 0 && static_cast<std::size_t>(written) < log.size()) {
                log.resize(static_cast<std::size_t>(written));
            }
            return log;
        }

        GLuint compile_shader(GLenum type, const char *source, const char *label) {
            ScopedInternalPass guard;
            const GLuint shader = g_glCreateShader(type);
            if (shader == 0) {
                LOG_ERROR("SpriteBodyFsr failed to create {} shader", label);
                return 0;
            }

            g_glShaderSource(shader, 1, &source, nullptr);
            g_glCompileShader(shader);

            GLint compiled = 0;
            g_glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled != 0) return shader;

            const auto log = shader_info_log(shader);
            LOG_ERROR("SpriteBodyFsr {} shader compile failed: {}", label, log.empty() ? "<empty>" : log);
            g_glDeleteShader(shader);
            return 0;
        }

        PassProgram link_program(const char *vertexSource,
                                 const char *fragmentSource,
                                 const char *label) {
            PassProgram out{};
            const auto vertexShader = compile_shader(GL_VERTEX_SHADER, vertexSource, "vertex");
            if (vertexShader == 0) return out;

            const auto fragmentShader = compile_shader(GL_FRAGMENT_SHADER, fragmentSource, label);
            if (fragmentShader == 0) {
                ScopedInternalPass guard;
                g_glDeleteShader(vertexShader);
                return out;
            }

            ScopedInternalPass guard;
            out.id = g_glCreateProgram();
            if (out.id == 0) {
                LOG_ERROR("SpriteBodyFsr failed to create {} program", label);
                g_glDeleteShader(fragmentShader);
                g_glDeleteShader(vertexShader);
                return out;
            }

            g_glAttachShader(out.id, vertexShader);
            g_glAttachShader(out.id, fragmentShader);
            g_glLinkProgram(out.id);

            GLint linked = 0;
            g_glGetProgramiv(out.id, GL_LINK_STATUS, &linked);
            g_glDeleteShader(fragmentShader);
            g_glDeleteShader(vertexShader);
            if (linked == 0) {
                const auto log = program_info_log(out.id);
                LOG_ERROR("SpriteBodyFsr {} program link failed: {}", label, log.empty() ? "<empty>" : log);
                g_glDeleteProgram(out.id);
                out = PassProgram{};
                return out;
            }

            out.uTex = g_glGetUniformLocation(out.id, "uTex");
            out.uInputSize = g_glGetUniformLocation(out.id, "uInputSize");
            out.uOutputSize = g_glGetUniformLocation(out.id, "uOutputSize");
            out.uSharpness = g_glGetUniformLocation(out.id, "uRcasSharpness");
            return out;
        }

        bool ensure_pass_programs() {
            if (g_programsReady) return true;
            if (!ensure_gl_runtime()) return false;

            static constexpr char kFullscreenVertexShader[] = R"GLSL(
#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

varying mediump vec2 vTexCoord;

void main()
{
    gl_Position = gl_Vertex;
    vTexCoord = gl_MultiTexCoord0.xy;
}
)GLSL";

            static constexpr char kBlitFragmentShader[] = R"GLSL(
#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

uniform lowp sampler2D uTex;
varying mediump vec2 vTexCoord;

void main()
{
    gl_FragColor = texture2D(uTex, vTexCoord);
}
)GLSL";

            static constexpr char kEasuFragmentShader[] = R"GLSL(
#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

uniform lowp sampler2D uTex;
uniform highp vec2 uInputSize;
uniform highp vec2 uOutputSize;
varying mediump vec2 vTexCoord;

mediump float ieeLuma(in lowp vec3 c)
{
    return c.g + 0.5 * (c.r + c.b);
}

lowp vec4 ieeSampleSource(in mediump vec2 uv)
{
    mediump vec2 halfTexel = vec2(0.5) / uInputSize;
    return texture2D(uTex, clamp(uv, halfTexel, vec2(1.0) - halfTexel));
}

void ieeEasuCon(
    out vec4 con0,
    out vec4 con1,
    out vec4 con2,
    out vec4 con3)
{
    con0 = vec4(
        uInputSize.x / uOutputSize.x,
        uInputSize.y / uOutputSize.y,
        0.5 * uInputSize.x / uOutputSize.x - 0.5,
        0.5 * uInputSize.y / uOutputSize.y - 0.5
    );
    con1 = vec4(1.0, 1.0, 1.0, -1.0) / uInputSize.xyxy;
    con2 = vec4(-1.0, 2.0, 1.0, 2.0) / uInputSize.xyxy;
    con3 = vec4(0.0, 4.0, 0.0, 0.0) / uInputSize.xyxy;
}

void ieeEasuTap(
    inout mediump vec3 aC,
    inout mediump float aW,
    mediump vec2 off,
    mediump vec2 dir,
    mediump vec2 len,
    mediump float lob,
    mediump float clp,
    lowp vec3 c)
{
    mediump vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x)));
    v *= len;
    mediump float d2 = min(dot(v, v), clp);
    mediump float wB = 0.4 * d2 - 1.0;
    mediump float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    mediump float w = wB * wA;
    aC += c * w;
    aW += w;
}

void ieeEasuSet(
    inout mediump vec2 dir,
    inout mediump float len,
    mediump float w,
    mediump float lA,
    mediump float lB,
    mediump float lC,
    mediump float lD,
    mediump float lE)
{
    mediump float lenX = max(abs(lD - lC), abs(lC - lB));
    mediump float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) / max(lenX, 1.0e-5), 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;

    mediump float lenY = max(abs(lE - lC), abs(lC - lA));
    mediump float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) / max(lenY, 1.0e-5), 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void main()
{
    vec4 con0;
    vec4 con1;
    vec4 con2;
    vec4 con3;
    ieeEasuCon(con0, con1, con2, con3);

    mediump vec2 fragCoord = vTexCoord * uOutputSize;
    mediump vec2 pp = fragCoord * con0.xy + con0.zw;
    mediump vec2 fp = floor(pp);
    pp -= fp;

    mediump vec2 p0 = fp * con1.xy + con1.zw;
    mediump vec2 p1 = p0 + con2.xy;
    mediump vec2 p2 = p0 + con2.zw;
    mediump vec2 p3 = p0 + con3.xy;

    mediump vec4 off = vec4(-0.5, 0.5, -0.5, 0.5) * con1.xxyy;

    lowp vec4 b = ieeSampleSource(p0 + off.xw); mediump float bL = ieeLuma(b.rgb);
    lowp vec4 c = ieeSampleSource(p0 + off.yw); mediump float cL = ieeLuma(c.rgb);
    lowp vec4 i = ieeSampleSource(p1 + off.xw); mediump float iL = ieeLuma(i.rgb);
    lowp vec4 j = ieeSampleSource(p1 + off.yw); mediump float jL = ieeLuma(j.rgb);
    lowp vec4 f = ieeSampleSource(p1 + off.yz); mediump float fL = ieeLuma(f.rgb);
    lowp vec4 e = ieeSampleSource(p1 + off.xz); mediump float eL = ieeLuma(e.rgb);
    lowp vec4 k = ieeSampleSource(p2 + off.xw); mediump float kL = ieeLuma(k.rgb);
    lowp vec4 l = ieeSampleSource(p2 + off.yw); mediump float lL = ieeLuma(l.rgb);
    lowp vec4 h = ieeSampleSource(p2 + off.yz); mediump float hL = ieeLuma(h.rgb);
    lowp vec4 g = ieeSampleSource(p2 + off.xz); mediump float gL = ieeLuma(g.rgb);
    lowp vec4 o = ieeSampleSource(p3 + off.yz); mediump float oL = ieeLuma(o.rgb);
    lowp vec4 n = ieeSampleSource(p3 + off.xz); mediump float nL = ieeLuma(n.rgb);

    mediump vec2 dir = vec2(0.0);
    mediump float len = 0.0;
    ieeEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    ieeEasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    ieeEasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    ieeEasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    mediump float dirR = dot(dir, dir);
    bool zro = dirR < (1.0 / 32768.0);
    dirR = inversesqrt(max(dirR, 1.0 / 32768.0));
    dir = zro ? vec2(1.0, 0.0) : dir * dirR;

    len = 0.5 * len;
    len *= len;
    mediump float stretch = dot(dir, dir) / max(max(abs(dir.x), abs(dir.y)), 1.0e-5);
    mediump vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    mediump float lob = 0.5 - 0.29 * len;
    mediump float clp = 1.0 / lob;

    lowp vec3 min4 = min(min(f.rgb, g.rgb), min(j.rgb, k.rgb));
    lowp vec3 max4 = max(max(f.rgb, g.rgb), max(j.rgb, k.rgb));
    mediump vec3 aC = vec3(0.0);
    mediump float aW = 0.0;
    ieeEasuTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b.rgb);
    ieeEasuTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c.rgb);
    ieeEasuTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i.rgb);
    ieeEasuTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j.rgb);
    ieeEasuTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f.rgb);
    ieeEasuTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e.rgb);
    ieeEasuTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k.rgb);
    ieeEasuTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l.rgb);
    ieeEasuTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h.rgb);
    ieeEasuTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g.rgb);
    ieeEasuTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o.rgb);
    ieeEasuTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n.rgb);

    lowp vec3 rgb = min(max4, max(min4, aC / max(aW, 1.0e-5)));
    lowp float alpha = ieeSampleSource(vTexCoord).a;
    gl_FragColor = vec4(rgb, alpha);
}
)GLSL";

            static constexpr char kRcasFragmentShader[] = R"GLSL(
#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

uniform lowp sampler2D uTex;
uniform highp vec2 uInputSize;
uniform highp vec2 uOutputSize;
uniform highp float uRcasSharpness;
varying mediump vec2 vTexCoord;

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

lowp vec4 ieeLoad(in mediump vec2 pixel)
{
    return texture2D(uTex, pixel / uOutputSize);
}

void main()
{
    mediump vec2 pixel = vTexCoord * uOutputSize;
    lowp vec3 b = ieeLoad(pixel + vec2( 0.0, -1.0)).rgb;
    lowp vec3 d = ieeLoad(pixel + vec2(-1.0,  0.0)).rgb;
    lowp vec3 e = ieeLoad(pixel).rgb;
    lowp vec3 f = ieeLoad(pixel + vec2( 1.0,  0.0)).rgb;
    lowp vec3 h = ieeLoad(pixel + vec2( 0.0,  1.0)).rgb;

    lowp vec3 mn4 = min(b, min(f, h));
    lowp vec3 mx4 = max(b, max(f, h));
    mediump vec3 hitMin = mn4 / max(4.0 * mx4, vec3(1.0e-5));
    mediump vec3 hitMax = (vec3(1.0) - mx4) / max(4.0 * mn4 - 4.0, vec3(-3.9999));
    mediump vec3 lobeRgb = max(-hitMin, hitMax);
    mediump float lobe = max(-FSR_RCAS_LIMIT,
                             min(max(lobeRgb.r, max(lobeRgb.g, lobeRgb.b)), 0.0)) * exp2(-uRcasSharpness);
    lowp vec3 rgb = (lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0);
    lowp float alpha = texture2D(uTex, vTexCoord).a;
    gl_FragColor = vec4(rgb, alpha);
}
)GLSL";

            g_blitProgram = link_program(kFullscreenVertexShader, kBlitFragmentShader, "blit");
            g_easuProgram = link_program(kFullscreenVertexShader, kEasuFragmentShader, "easu");
            g_rcasProgram = link_program(kFullscreenVertexShader, kRcasFragmentShader, "rcas");
            g_programsReady = g_blitProgram.id != 0 && g_easuProgram.id != 0 && g_rcasProgram.id != 0;
            if (!g_programsReady) {
                LOG_ERROR("SpriteBodyFsr failed to compile internal pass shaders");
                destroy_pass_program(g_blitProgram);
                destroy_pass_program(g_easuProgram);
                destroy_pass_program(g_rcasProgram);
            }
            return g_programsReady;
        }

        bool create_texture_and_fbo(GLuint &texture,
                                    GLuint &fbo,
                                    GLint width,
                                    GLint height,
                                    const char *label) {
            ScopedInternalPass guard;
            if (texture == 0) {
                g_glGenTextures(1, &texture);
            }
            if (texture == 0) {
                LOG_ERROR("SpriteBodyFsr failed to allocate {} texture", label);
                return false;
            }

            g_glBindTexture(GL_TEXTURE_2D, texture);
            g_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            g_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            g_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

            if (fbo == 0) {
                g_glGenFramebuffers(1, &fbo);
            }
            if (fbo == 0) {
                LOG_ERROR("SpriteBodyFsr failed to allocate {} framebuffer", label);
                return false;
            }

            g_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            g_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            if (g_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                LOG_ERROR("SpriteBodyFsr {} framebuffer is incomplete", label);
                return false;
            }
            return true;
        }

        bool ensure_capture_resources() {
            if (!ensure_gl_runtime()) return false;

            const auto requestedDisplayWidth = clamp_positive(g_displayViewport[2]);
            const auto requestedDisplayHeight = clamp_positive(g_displayViewport[3]);
            const auto requestedInputWidth = scaled_dimension(requestedDisplayWidth, g_inputScale);
            const auto requestedInputHeight = scaled_dimension(requestedDisplayHeight, g_inputScale);

            if (g_spriteInputTex != 0 &&
                g_spriteUpscaleTex != 0 &&
                g_inputWidth == requestedInputWidth &&
                g_inputHeight == requestedInputHeight &&
                g_displayWidth == requestedDisplayWidth &&
                g_displayHeight == requestedDisplayHeight) {
                return true;
            }

            destroy_resources();

            if (!create_texture_and_fbo(g_spriteInputTex,
                                        g_spriteInputFbo,
                                        requestedInputWidth,
                                        requestedInputHeight,
                                        "sprite input")) {
                destroy_resources();
                return false;
            }

            if (!create_texture_and_fbo(g_spriteUpscaleTex,
                                        g_spriteUpscaleFbo,
                                        requestedDisplayWidth,
                                        requestedDisplayHeight,
                                        "sprite upscale")) {
                destroy_resources();
                return false;
            }

            {
                ScopedInternalPass guard;
                g_glBindFramebuffer(GL_FRAMEBUFFER, g_displayFramebuffer);
            }

            g_inputWidth = requestedInputWidth;
            g_inputHeight = requestedInputHeight;
            g_displayWidth = requestedDisplayWidth;
            g_displayHeight = requestedDisplayHeight;

            LOG_INFO("SpriteBodyFsr allocated resources input={}x{} output={}x{}",
                     g_inputWidth,
                     g_inputHeight,
                     g_displayWidth,
                     g_displayHeight);
            return true;
        }

        void draw_fullscreen_quad() {
            g_glBegin(GL_TRIANGLE_STRIP);
            g_glTexCoord2f(0.0f, 0.0f);
            g_glVertex2f(-1.0f, -1.0f);
            g_glTexCoord2f(1.0f, 0.0f);
            g_glVertex2f(1.0f, -1.0f);
            g_glTexCoord2f(0.0f, 1.0f);
            g_glVertex2f(-1.0f, 1.0f);
            g_glTexCoord2f(1.0f, 1.0f);
            g_glVertex2f(1.0f, 1.0f);
            g_glEnd();
        }

        void set_program_uniforms(const PassProgram &program,
                                  GLuint texture,
                                  GLfloat inputWidth,
                                  GLfloat inputHeight,
                                  GLfloat outputWidth,
                                  GLfloat outputHeight,
                                  GLfloat sharpness = 0.0f) {
            g_glUseProgram(program.id);
            if (program.uTex >= 0) g_glUniform1i(program.uTex, 0);
            if (program.uInputSize >= 0) g_glUniform2f(program.uInputSize, inputWidth, inputHeight);
            if (program.uOutputSize >= 0) g_glUniform2f(program.uOutputSize, outputWidth, outputHeight);
            if (program.uSharpness >= 0) g_glUniform1f(program.uSharpness, sharpness);
            g_glActiveTexture(GL_TEXTURE0);
            g_glBindTexture(GL_TEXTURE_2D, texture);
        }

        void bind_display_target_for_composite() {
            g_glBindFramebuffer(GL_FRAMEBUFFER, g_displayFramebuffer);
            g_glViewport(g_displayViewport[0], g_displayViewport[1], g_displayViewport[2], g_displayViewport[3]);
            if (g_displayScissorEnabled) {
                g_glEnable(GL_SCISSOR_TEST);
                g_glScissor(g_displayScissor[0], g_displayScissor[1], g_displayScissor[2], g_displayScissor[3]);
            } else {
                g_glDisable(GL_SCISSOR_TEST);
            }
            g_glEnable(GL_BLEND);
            g_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            if (g_glBlendEquation) {
                g_glBlendEquation(GL_FUNC_ADD);
            }
        }

        void restore_display_draw_state(GLint savedProgram,
                                        GLint savedActiveTexture,
                                        GLint savedTextureUnit0) {
            g_glBindFramebuffer(GL_FRAMEBUFFER, g_displayFramebuffer);
            g_glViewport(g_displayViewport[0], g_displayViewport[1], g_displayViewport[2], g_displayViewport[3]);

            if (g_displayScissorEnabled) {
                g_glEnable(GL_SCISSOR_TEST);
                g_glScissor(g_displayScissor[0], g_displayScissor[1], g_displayScissor[2], g_displayScissor[3]);
            } else {
                g_glDisable(GL_SCISSOR_TEST);
            }

            if (g_displayBlendEnabled) {
                g_glEnable(GL_BLEND);
                g_glBlendFunc(static_cast<GLenum>(g_displayBlendSrcRgb),
                              static_cast<GLenum>(g_displayBlendDstRgb));
                if (g_glBlendEquation) {
                    g_glBlendEquation(static_cast<GLenum>(g_displayBlendEquationRgb));
                }
            } else {
                g_glDisable(GL_BLEND);
            }

            g_glActiveTexture(GL_TEXTURE0);
            g_glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(std::max(savedTextureUnit0, 0)));
            g_glActiveTexture(static_cast<GLenum>(savedActiveTexture));
            g_glUseProgram(static_cast<GLuint>(std::max(savedProgram, 0)));
        }

        void flush_pending_layer() {
            if (!g_capturePending) return;
            if (!ensure_gl_runtime()) {
                g_capturePending = false;
                return;
            }
            if (!ensure_capture_resources()) {
                g_capturePending = false;
                return;
            }
            if (!ensure_pass_programs()) {
                g_capturePending = false;
                return;
            }

            GLint savedProgram = 0;
            GLint savedActiveTexture = GL_TEXTURE0;
            GLint savedTextureUnit0 = 0;
            g_glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
            g_glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture);
            {
                ScopedInternalPass guard;
                g_glActiveTexture(GL_TEXTURE0);
                g_glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTextureUnit0);
            }

            {
                ScopedInternalPass guard;

                if (g_debugView == 2 || g_debugView == 3 || g_debugView == 0) {
                    g_glBindFramebuffer(GL_FRAMEBUFFER, g_spriteUpscaleFbo);
                    g_glViewport(0, 0, g_displayWidth, g_displayHeight);
                    g_glDisable(GL_SCISSOR_TEST);
                    g_glDisable(GL_BLEND);
                    set_program_uniforms(g_easuProgram,
                                         g_spriteInputTex,
                                         static_cast<GLfloat>(g_inputWidth),
                                         static_cast<GLfloat>(g_inputHeight),
                                         static_cast<GLfloat>(g_displayWidth),
                                         static_cast<GLfloat>(g_displayHeight));
                    draw_fullscreen_quad();
                }

                bind_display_target_for_composite();
                if (g_debugView == 1) {
                    set_program_uniforms(g_blitProgram,
                                         g_spriteInputTex,
                                         static_cast<GLfloat>(g_inputWidth),
                                         static_cast<GLfloat>(g_inputHeight),
                                         static_cast<GLfloat>(g_displayWidth),
                                         static_cast<GLfloat>(g_displayHeight));
                } else if (g_debugView == 2) {
                    set_program_uniforms(g_blitProgram,
                                         g_spriteUpscaleTex,
                                         static_cast<GLfloat>(g_displayWidth),
                                         static_cast<GLfloat>(g_displayHeight),
                                         static_cast<GLfloat>(g_displayWidth),
                                         static_cast<GLfloat>(g_displayHeight));
                } else if (g_enableRcas) {
                    set_program_uniforms(g_rcasProgram,
                                         g_spriteUpscaleTex,
                                         static_cast<GLfloat>(g_displayWidth),
                                         static_cast<GLfloat>(g_displayHeight),
                                         static_cast<GLfloat>(g_displayWidth),
                                         static_cast<GLfloat>(g_displayHeight),
                                         g_rcasSharpness);
                } else {
                    set_program_uniforms(g_blitProgram,
                                         g_spriteUpscaleTex,
                                         static_cast<GLfloat>(g_displayWidth),
                                         static_cast<GLfloat>(g_displayHeight),
                                         static_cast<GLfloat>(g_displayWidth),
                                         static_cast<GLfloat>(g_displayHeight));
                }
                draw_fullscreen_quad();

                restore_display_draw_state(savedProgram, savedActiveTexture, savedTextureUnit0);
            }

            g_capturePending = false;
        }

        bool matches_target(const DrawCallInfo &info) noexcept {
            return g_enabled &&
                   info.program == g_targetProgram &&
                   info.texture == g_targetTexture &&
                   info.framebuffer == 0;
        }

        bool begin_capture_if_needed() {
            if (!ensure_gl_runtime()) return false;

            if (!g_capturePending) {
                query_display_state();
                if (!ensure_capture_resources()) {
                    return false;
                }

                std::array<GLint, 4> scaledViewport{};
                std::array<GLint, 4> scaledScissor{};
                scale_rect(g_displayViewport,
                           g_displayWidth,
                           g_displayHeight,
                           g_inputWidth,
                           g_inputHeight,
                           scaledViewport);
                scale_rect(g_displayScissor,
                           g_displayWidth,
                           g_displayHeight,
                           g_inputWidth,
                           g_inputHeight,
                           scaledScissor);

                ScopedInternalPass guard;
                g_glBindFramebuffer(GL_FRAMEBUFFER, g_spriteInputFbo);
                g_glViewport(scaledViewport[0], scaledViewport[1], scaledViewport[2], scaledViewport[3]);
                if (g_displayScissorEnabled) {
                    g_glEnable(GL_SCISSOR_TEST);
                    g_glScissor(scaledScissor[0], scaledScissor[1], scaledScissor[2], scaledScissor[3]);
                } else {
                    g_glDisable(GL_SCISSOR_TEST);
                }
                g_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                g_glClear(GL_COLOR_BUFFER_BIT);
                g_capturePending = true;
            }

            return true;
        }

        template<typename DrawFn, typename... Args>
        bool capture_draw_if_targeted(const DrawCallInfo &info, DrawFn original, Args... args) {
            if (!g_enabled) return false;

            if (!matches_target(info)) {
                flush_pending_layer();
                return false;
            }

            if (!begin_capture_if_needed()) {
                return false;
            }

            original(args...);
            return true;
        }

        BOOL WINAPI Detour_SwapBuffers(HDC hdc) {
            if (!g_enabled || g_internalPassActive) {
                return H_SwapBuffers.original()(hdc);
            }

            flush_pending_layer();
            return H_SwapBuffers.original()(hdc);
        }
    }

    bool install(const core::EngineConfig &cfg) {
        g_enabled = cfg.enableSpriteBodyFsrPrototype;
        g_targetProgram = static_cast<GLuint>(std::max(cfg.spriteBodyProgram, 0));
        g_targetTexture = static_cast<GLuint>(std::max(cfg.spriteBodyTexture, 0));
        g_inputScale = std::clamp(cfg.spriteBodyInputScale, 0.1f, 1.0f);
        g_enableRcas = cfg.spriteBodyEnableRcas;
        g_rcasSharpness = std::clamp(cfg.spriteBodyRcasSharpness, 0.0f, 1.0f);
        g_debugView = std::clamp(cfg.spriteBodyDebugView, 0, 3);

        if (!g_enabled) return true;
        if (g_installed) return true;

        static core::HookInit hookInit;

        auto *gdi32 = GetModuleHandleA("gdi32.dll");
        if (!gdi32) {
            gdi32 = LoadLibraryA("gdi32.dll");
        }
        if (!gdi32) {
            LOG_ERROR("SpriteBodyFsr failed to load gdi32.dll");
            return false;
        }

        auto *swapBuffersTarget = reinterpret_cast<void *>(GetProcAddress(gdi32, "SwapBuffers"));
        if (!swapBuffersTarget) {
            LOG_ERROR("SpriteBodyFsr failed to locate SwapBuffers");
            return false;
        }

        try {
            H_SwapBuffers.create(swapBuffersTarget, reinterpret_cast<void *>(&Detour_SwapBuffers));
            H_SwapBuffers.enable();
            g_installed = true;
            LOG_INFO("SpriteBodyFsr prototype enabled for program={} texture={} inputScale={:.3f} rcas={} sharpness={:.2f} debugView={}",
                     g_targetProgram,
                     g_targetTexture,
                     g_inputScale,
                     g_enableRcas ? "true" : "false",
                     g_rcasSharpness,
                     g_debugView);
            return true;
        } catch (const std::exception &e) {
            LOG_ERROR("SpriteBodyFsr install failed: {}", e.what());
            return false;
        }
    }

    bool is_internal_pass_active() noexcept {
        return g_internalPassActive;
    }

    void on_external_bind_framebuffer(GLenum target, GLuint framebuffer) {
        if (!g_enabled || g_internalPassActive) return;
        if (!g_capturePending) return;
        if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER) return;
        if (framebuffer == g_spriteInputFbo || framebuffer == g_spriteUpscaleFbo) return;
        flush_pending_layer();
    }

    void reset_runtime_state(const char *reason) noexcept {
        (void) reason;
        g_capturePending = false;
    }

    bool handle_draw_arrays(const DrawCallInfo &info,
                            GLenum mode,
                            GLint first,
                            GLsizei count,
                            DrawArraysFn original) {
        return capture_draw_if_targeted(info, original, mode, first, count);
    }

    bool handle_draw_elements(const DrawCallInfo &info,
                              GLenum mode,
                              GLsizei count,
                              GLenum type,
                              const void *indices,
                              DrawElementsFn original) {
        return capture_draw_if_targeted(info, original, mode, count, type, indices);
    }

    void uninstall() noexcept {
        if (!g_installed) return;

        flush_pending_layer();
        H_SwapBuffers.disable();
        destroy_pass_program(g_blitProgram);
        destroy_pass_program(g_easuProgram);
        destroy_pass_program(g_rcasProgram);
        destroy_resources();

        g_programsReady = false;
        g_installed = false;
        g_enabled = false;
        g_targetProgram = 3;
        g_targetTexture = 2;
        g_inputScale = 0.667f;
        g_enableRcas = true;
        g_rcasSharpness = 0.20f;
        g_debugView = 0;
        g_capturePending = false;
    }
}
