#include "opengl_types.h"
#include "iee/core/logger.h"
#include <windows.h>
#include <mutex>

namespace iee::game::gl {
    // region Error Handling Utilities
    const char *error_string(unsigned error_code) noexcept {
        switch (error_code) {
            case GL_NO_ERROR: return "GL_NO_ERROR";
            case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
            case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
            case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
            case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
            case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
            case GL_CONTEXT_LOST: return "GL_CONTEXT_LOST";
            default: return "UNKNOWN_GL_ERROR";
        }
    }

    bool check_error(const char *operation) noexcept {
        const auto &gl = get_gl_functions();
        if (!gl.glGetError) return true; // Can't check errors without glGetError

        if (unsigned error = gl.glGetError(); error != GL_NO_ERROR) {
            LOG_WARN("OpenGL error in {}: {} (0x{:X})", operation, error_string(error), error);
            return false;
        }
        return true;
    }

    // endregion

    // region Function Loading

    // Load a GL1.1 core entry point from opengl32.dll directly.
    // wglGetProcAddress returns NULL for GL1.1 core symbols, so we must use
    // GetProcAddress on the opengl32.dll module handle instead.
    static void *get_gl1_proc_address(HMODULE opengl32, const char *name) noexcept {
        if (!opengl32) return nullptr;
        return GetProcAddress(opengl32, name);
    }

    // Load an extension entry point.  Try wglGetProcAddress first (correct path
    // for ARB/core-profile extensions); fall back to opengl32.dll GetProcAddress
    // for any symbol that happens to be exported there (e.g. on some drivers).
    static void *get_ext_proc_address(HMODULE opengl32, const char *name) noexcept {
        if (!opengl32) return nullptr;

        using PFN_wglGetProcAddress = void *(WINAPI *)(const char *);
        static auto wglGetProcAddr = reinterpret_cast<PFN_wglGetProcAddress>(
            GetProcAddress(opengl32, "wglGetProcAddress")
        );

        if (wglGetProcAddr) {
            if (void *proc = wglGetProcAddr(name)) return proc;
        }
        return GetProcAddress(opengl32, name);
    }

    HGLRC current_context() noexcept {
        static HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
        if (!opengl32) return nullptr;

        using PFN_wglGetCurrentContext = HGLRC (WINAPI *)();
        static auto wglGetCurrentContext = reinterpret_cast<PFN_wglGetCurrentContext>(
            GetProcAddress(opengl32, "wglGetCurrentContext")
        );

        return wglGetCurrentContext ? wglGetCurrentContext() : nullptr;
    }

    bool OpenGLFunctions::initialize() noexcept {
        static HMODULE opengl32 = GetModuleHandleA("opengl32.dll");

        // --- GL1.1 core: load via opengl32.dll GetProcAddress ---
        glTexParameteri   = reinterpret_cast<PFN_glTexParameteri>  (get_gl1_proc_address(opengl32, "glTexParameteri"));
        glTexParameterf   = reinterpret_cast<PFN_glTexParameterf>  (get_gl1_proc_address(opengl32, "glTexParameterf"));
        glGetIntegerv     = reinterpret_cast<PFN_glGetIntegerv>    (get_gl1_proc_address(opengl32, "glGetIntegerv"));
        glGetTexParameteriv = reinterpret_cast<PFN_glGetTexParameteriv>(get_gl1_proc_address(opengl32, "glGetTexParameteriv"));
        glGetError        = reinterpret_cast<PFN_glGetError>       (get_gl1_proc_address(opengl32, "glGetError"));
        glGenTextures     = reinterpret_cast<PFN_glGenTextures>    (get_gl1_proc_address(opengl32, "glGenTextures"));
        glBindTexture     = reinterpret_cast<PFN_glBindTexture>    (get_gl1_proc_address(opengl32, "glBindTexture"));
        glTexImage2D      = reinterpret_cast<PFN_glTexImage2D>     (get_gl1_proc_address(opengl32, "glTexImage2D"));
        glDeleteTextures  = reinterpret_cast<PFN_glDeleteTextures> (get_gl1_proc_address(opengl32, "glDeleteTextures"));
        glPixelStorei     = reinterpret_cast<PFN_glPixelStorei>    (get_gl1_proc_address(opengl32, "glPixelStorei"));

        glGetString       = reinterpret_cast<PFN_glGetString>      (get_gl1_proc_address(opengl32, "glGetString"));

        // --- Extensions: load via wglGetProcAddress ---
        glCreateShader    = reinterpret_cast<PFN_glCreateShader>   (get_ext_proc_address(opengl32, "glCreateShader"));
        glShaderSource    = reinterpret_cast<PFN_glShaderSource>   (get_ext_proc_address(opengl32, "glShaderSource"));
        glCompileShader   = reinterpret_cast<PFN_glCompileShader>  (get_ext_proc_address(opengl32, "glCompileShader"));
        glDeleteShader    = reinterpret_cast<PFN_glDeleteShader>   (get_ext_proc_address(opengl32, "glDeleteShader"));
        glGetShaderiv     = reinterpret_cast<PFN_glGetShaderiv>    (get_ext_proc_address(opengl32, "glGetShaderiv"));
        glGetShaderInfoLog = reinterpret_cast<PFN_glGetShaderInfoLog>(get_ext_proc_address(opengl32, "glGetShaderInfoLog"));
        glCreateProgram   = reinterpret_cast<PFN_glCreateProgram>  (get_ext_proc_address(opengl32, "glCreateProgram"));
        glAttachShader    = reinterpret_cast<PFN_glAttachShader>   (get_ext_proc_address(opengl32, "glAttachShader"));
        glLinkProgram     = reinterpret_cast<PFN_glLinkProgram>    (get_ext_proc_address(opengl32, "glLinkProgram"));
        glUseProgram      = reinterpret_cast<PFN_glUseProgram>     (get_ext_proc_address(opengl32, "glUseProgram"));
        glDeleteProgram   = reinterpret_cast<PFN_glDeleteProgram>  (get_ext_proc_address(opengl32, "glDeleteProgram"));
        glGetProgramiv    = reinterpret_cast<PFN_glGetProgramiv>   (get_ext_proc_address(opengl32, "glGetProgramiv"));
        glGetProgramInfoLog = reinterpret_cast<PFN_glGetProgramInfoLog>(get_ext_proc_address(opengl32, "glGetProgramInfoLog"));
        glGetAttachedShaders = reinterpret_cast<PFN_glGetAttachedShaders>(get_ext_proc_address(opengl32, "glGetAttachedShaders"));
        glGetShaderSource = reinterpret_cast<PFN_glGetShaderSource>(get_ext_proc_address(opengl32, "glGetShaderSource"));
        glGetUniformLocation = reinterpret_cast<PFN_glGetUniformLocation>(get_ext_proc_address(opengl32, "glGetUniformLocation"));
        glUniform1f       = reinterpret_cast<PFN_glUniform1f>      (get_ext_proc_address(opengl32, "glUniform1f"));
        glUniform1i       = reinterpret_cast<PFN_glUniform1i>      (get_ext_proc_address(opengl32, "glUniform1i"));
        glUniform2f       = reinterpret_cast<PFN_glUniform2f>      (get_ext_proc_address(opengl32, "glUniform2f"));
        glActiveTexture   = reinterpret_cast<PFN_glActiveTexture>  (get_ext_proc_address(opengl32, "glActiveTexture"));
        glBindFramebuffer = reinterpret_cast<PFN_glBindFramebuffer>(get_ext_proc_address(opengl32, "glBindFramebuffer"));
        glShaderSourceARB = reinterpret_cast<PFN_glShaderSourceARB>(get_ext_proc_address(opengl32, "glShaderSourceARB"));
        glCompileShaderARB = reinterpret_cast<PFN_glCompileShaderARB>(get_ext_proc_address(opengl32, "glCompileShaderARB"));
        glLinkProgramARB  = reinterpret_cast<PFN_glLinkProgramARB> (get_ext_proc_address(opengl32, "glLinkProgramARB"));
        glUseProgramObjectARB = reinterpret_cast<PFN_glUseProgramObjectARB>(get_ext_proc_address(opengl32, "glUseProgramObjectARB"));

        // --- Capability flags ---
        valid = (glTexParameteri    != nullptr &&
                 glTexParameterf    != nullptr &&
                 glGetIntegerv      != nullptr &&
                 glGetTexParameteriv != nullptr &&
                 glGetError         != nullptr);

        shaderObjectsAvailable =
            glCreateShader  != nullptr &&
            glShaderSource  != nullptr &&
            glCompileShader != nullptr &&
            glDeleteShader  != nullptr &&
            glCreateProgram != nullptr &&
            glAttachShader  != nullptr &&
            glLinkProgram   != nullptr &&
            glUseProgram    != nullptr &&
            glDeleteProgram != nullptr;

        shaderIntrospectionAvailable =
            glGetShaderiv       != nullptr &&
            glGetShaderInfoLog  != nullptr &&
            glGetProgramiv      != nullptr &&
            glGetProgramInfoLog != nullptr &&
            glGetAttachedShaders != nullptr &&
            glGetShaderSource   != nullptr;

        uniformApiAvailable =
            glGetUniformLocation != nullptr &&
            glUniform1f          != nullptr &&
            glUniform1i          != nullptr;

        readyForSourcePatching =
            shaderObjectsAvailable &&
            shaderIntrospectionAvailable &&
            uniformApiAvailable;

        arbShaderObjectsAvailable =
            glShaderSourceARB     != nullptr &&
            glCompileShaderARB    != nullptr &&
            glLinkProgramARB      != nullptr &&
            glUseProgramObjectARB != nullptr;

        textureUploadAvailable =
            glGenTextures    != nullptr &&
            glBindTexture    != nullptr &&
            glTexImage2D     != nullptr &&
            glDeleteTextures != nullptr &&
            glPixelStorei    != nullptr &&
            glActiveTexture  != nullptr;

        if (valid) {
            LOG_INFO("OpenGL functions initialized successfully:");
            LOG_INFO("  glGetString: {}", glGetString ? "OK" : "MISSING");
            LOG_INFO("  glTexParameteri: {}", glTexParameteri ? "OK" : "MISSING");
            LOG_INFO("  glTexParameterf: {}", glTexParameterf ? "OK" : "MISSING");
            LOG_INFO("  glGetIntegerv: {}", glGetIntegerv ? "OK" : "MISSING");
            LOG_INFO("  glGetTexParameteriv: {}", glGetTexParameteriv ? "OK" : "MISSING");
            LOG_INFO("  glGetError: {}", glGetError ? "OK" : "MISSING");
            LOG_INFO("  glGenTextures: {}", glGenTextures ? "OK" : "MISSING");
            LOG_INFO("  glBindTexture: {}", glBindTexture ? "OK" : "MISSING");
            LOG_INFO("  glTexImage2D: {}", glTexImage2D ? "OK" : "MISSING");
            LOG_INFO("  glDeleteTextures: {}", glDeleteTextures ? "OK" : "MISSING");
            LOG_INFO("  glPixelStorei: {}", glPixelStorei ? "OK" : "MISSING");
            LOG_INFO("  Shader object API: {}", shaderObjectsAvailable ? "READY" : "PARTIAL");
            LOG_INFO("  Shader introspection API: {}", shaderIntrospectionAvailable ? "READY" : "PARTIAL");
            LOG_INFO("  Uniform API: {}", uniformApiAvailable ? "READY" : "PARTIAL");
            LOG_INFO("  Source patching readiness: {}", readyForSourcePatching ? "READY" : "PARTIAL");
            LOG_INFO("  ARB shader API: {}", arbShaderObjectsAvailable ? "READY" : "PARTIAL");
            LOG_INFO("  Texture upload API: {}", textureUploadAvailable ? "READY" : "PARTIAL");
        } else {
            LOG_ERROR("Failed to initialize some OpenGL functions");
        }

        return valid;
    }

    OpenGLFunctions &get_gl_functions() noexcept {
        static OpenGLFunctions instance;
        static std::mutex mutex;
        static HGLRC lastContext = nullptr;

        // Context read happens before the lock: a context switch in that window only
        // costs one spurious re-init on the next call (single render thread in practice).
        const auto context = current_context();
        std::lock_guard lock(mutex);
        if (!instance.valid || context != lastContext) {
            instance.initialize();
            lastContext = context;
        }

        return instance;
    }

    // endregion
}
