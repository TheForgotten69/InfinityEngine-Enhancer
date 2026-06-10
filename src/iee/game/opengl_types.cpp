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
    void *get_proc_address(const char *name) noexcept {
        static HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
        if (!opengl32) return nullptr;

        if (void *proc = GetProcAddress(opengl32, name)) return proc;

        using PFN_wglGetProcAddress = void* (WINAPI*)(const char *);
        static auto wglGetProcAddress = reinterpret_cast<PFN_wglGetProcAddress>(
            GetProcAddress(opengl32, "wglGetProcAddress")
        );

        return wglGetProcAddress ? wglGetProcAddress(name) : nullptr;
    }

    HGLRC current_context() noexcept {
        static HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
        if (!opengl32) {
            return nullptr;
        }

        using PFN_wglGetCurrentContext = HGLRC(WINAPI*)();
        static auto wglGetCurrentContext = reinterpret_cast<PFN_wglGetCurrentContext>(
            GetProcAddress(opengl32, "wglGetCurrentContext")
        );

        return wglGetCurrentContext ? wglGetCurrentContext() : nullptr;
    }

    bool OpenGLFunctions::initialize() noexcept {
        glGetString = reinterpret_cast<PFN_glGetString>(get_proc_address("glGetString"));
        glTexParameteri = reinterpret_cast<PFN_glTexParameteri>(get_proc_address("glTexParameteri"));
        glTexParameterf = reinterpret_cast<PFN_glTexParameterf>(get_proc_address("glTexParameterf"));
        glGetIntegerv = reinterpret_cast<PFN_glGetIntegerv>(get_proc_address("glGetIntegerv"));
        glGetTexParameteriv = reinterpret_cast<PFN_glGetTexParameteriv>(get_proc_address("glGetTexParameteriv"));
        glGetError = reinterpret_cast<PFN_glGetError>(get_proc_address("glGetError"));
        glCreateShader = reinterpret_cast<PFN_glCreateShader>(get_proc_address("glCreateShader"));
        glShaderSource = reinterpret_cast<PFN_glShaderSource>(get_proc_address("glShaderSource"));
        glCompileShader = reinterpret_cast<PFN_glCompileShader>(get_proc_address("glCompileShader"));
        glDeleteShader = reinterpret_cast<PFN_glDeleteShader>(get_proc_address("glDeleteShader"));
        glGetShaderiv = reinterpret_cast<PFN_glGetShaderiv>(get_proc_address("glGetShaderiv"));
        glGetShaderInfoLog = reinterpret_cast<PFN_glGetShaderInfoLog>(get_proc_address("glGetShaderInfoLog"));
        glCreateProgram = reinterpret_cast<PFN_glCreateProgram>(get_proc_address("glCreateProgram"));
        glAttachShader = reinterpret_cast<PFN_glAttachShader>(get_proc_address("glAttachShader"));
        glLinkProgram = reinterpret_cast<PFN_glLinkProgram>(get_proc_address("glLinkProgram"));
        glUseProgram = reinterpret_cast<PFN_glUseProgram>(get_proc_address("glUseProgram"));
        glDeleteProgram = reinterpret_cast<PFN_glDeleteProgram>(get_proc_address("glDeleteProgram"));
        glGetProgramiv = reinterpret_cast<PFN_glGetProgramiv>(get_proc_address("glGetProgramiv"));
        glGetProgramInfoLog = reinterpret_cast<PFN_glGetProgramInfoLog>(get_proc_address("glGetProgramInfoLog"));
        glGetAttachedShaders = reinterpret_cast<PFN_glGetAttachedShaders>(get_proc_address("glGetAttachedShaders"));
        glGetShaderSource = reinterpret_cast<PFN_glGetShaderSource>(get_proc_address("glGetShaderSource"));
        glGetUniformLocation = reinterpret_cast<PFN_glGetUniformLocation>(get_proc_address("glGetUniformLocation"));
        glUniform1f = reinterpret_cast<PFN_glUniform1f>(get_proc_address("glUniform1f"));
        glUniform1i = reinterpret_cast<PFN_glUniform1i>(get_proc_address("glUniform1i"));
        glShaderSourceARB = reinterpret_cast<PFN_glShaderSourceARB>(get_proc_address("glShaderSourceARB"));
        glCompileShaderARB = reinterpret_cast<PFN_glCompileShaderARB>(get_proc_address("glCompileShaderARB"));
        glLinkProgramARB = reinterpret_cast<PFN_glLinkProgramARB>(get_proc_address("glLinkProgramARB"));
        glUseProgramObjectARB = reinterpret_cast<PFN_glUseProgramObjectARB>(get_proc_address("glUseProgramObjectARB"));

        valid = (glTexParameteri != nullptr && glTexParameterf != nullptr &&
                 glGetIntegerv != nullptr && glGetTexParameteriv != nullptr &&
                 glGetError != nullptr);
        shaderObjectsAvailable =
            glCreateShader != nullptr &&
            glShaderSource != nullptr &&
            glCompileShader != nullptr &&
            glDeleteShader != nullptr &&
            glCreateProgram != nullptr &&
            glAttachShader != nullptr &&
            glLinkProgram != nullptr &&
            glUseProgram != nullptr &&
            glDeleteProgram != nullptr;
        shaderIntrospectionAvailable =
            glGetShaderiv != nullptr &&
            glGetShaderInfoLog != nullptr &&
            glGetProgramiv != nullptr &&
            glGetProgramInfoLog != nullptr &&
            glGetAttachedShaders != nullptr &&
            glGetShaderSource != nullptr;
        uniformApiAvailable =
            glGetUniformLocation != nullptr &&
            glUniform1f != nullptr &&
            glUniform1i != nullptr;
        readyForSourcePatching =
            shaderObjectsAvailable &&
            shaderIntrospectionAvailable &&
            uniformApiAvailable;
        arbShaderObjectsAvailable =
            glShaderSourceARB != nullptr &&
            glCompileShaderARB != nullptr &&
            glLinkProgramARB != nullptr &&
            glUseProgramObjectARB != nullptr;

        if (valid) {
            LOG_INFO("OpenGL functions initialized successfully:");
            LOG_INFO("  glGetString: {}", glGetString ? "OK" : "MISSING");
            LOG_INFO("  glTexParameteri: {}", glTexParameteri ? "OK" : "MISSING");
            LOG_INFO("  glTexParameterf: {}", glTexParameterf ? "OK" : "MISSING");
            LOG_INFO("  glGetIntegerv: {}", glGetIntegerv ? "OK" : "MISSING");
            LOG_INFO("  glGetTexParameteriv: {}", glGetTexParameteriv ? "OK" : "MISSING");
            LOG_INFO("  glGetError: {}", glGetError ? "OK" : "MISSING");
            LOG_INFO("  Shader object API: {}", shaderObjectsAvailable ? "READY" : "PARTIAL");
            LOG_INFO("  Shader introspection API: {}", shaderIntrospectionAvailable ? "READY" : "PARTIAL");
            LOG_INFO("  Uniform API: {}", uniformApiAvailable ? "READY" : "PARTIAL");
            LOG_INFO("  Source patching readiness: {}", readyForSourcePatching ? "READY" : "PARTIAL");
            LOG_INFO("  ARB shader API: {}", arbShaderObjectsAvailable ? "READY" : "PARTIAL");
        } else {
            LOG_ERROR("Failed to initialize some OpenGL functions");
        }

        return valid;
    }

    OpenGLFunctions &get_gl_functions() noexcept {
        static OpenGLFunctions instance;
        static std::mutex mutex;
        static HGLRC lastContext = nullptr;

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
