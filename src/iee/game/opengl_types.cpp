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

    bool OpenGLFunctions::initialize() noexcept {
        glTexParameteri = reinterpret_cast<PFN_glTexParameteri>(get_proc_address("glTexParameteri"));
        glTexParameterf = reinterpret_cast<PFN_glTexParameterf>(get_proc_address("glTexParameterf"));
        glGetIntegerv = reinterpret_cast<PFN_glGetIntegerv>(get_proc_address("glGetIntegerv"));
        glGetTexParameteriv = reinterpret_cast<PFN_glGetTexParameteriv>(get_proc_address("glGetTexParameteriv"));
        glGetError = reinterpret_cast<PFN_glGetError>(get_proc_address("glGetError"));

        valid = (glTexParameteri != nullptr && glTexParameterf != nullptr &&
                 glGetIntegerv != nullptr && glGetTexParameteriv != nullptr &&
                 glGetError != nullptr);

        if (valid) {
            LOG_INFO("OpenGL functions initialized successfully:");
            LOG_INFO("  glTexParameteri: {}", glTexParameteri ? "OK" : "MISSING");
            LOG_INFO("  glTexParameterf: {}", glTexParameterf ? "OK" : "MISSING");
            LOG_INFO("  glGetIntegerv: {}", glGetIntegerv ? "OK" : "MISSING");
            LOG_INFO("  glGetTexParameteriv: {}", glGetTexParameteriv ? "OK" : "MISSING");
            LOG_INFO("  glGetError: {}", glGetError ? "OK" : "MISSING");
        } else {
            LOG_ERROR("Failed to initialize some OpenGL functions");
        }

        return valid;
    }

    OpenGLFunctions &get_gl_functions() noexcept {
        static OpenGLFunctions instance;
        static std::once_flag initialized;

        std::call_once(initialized, [&]() {
            instance.initialize();
        });

        return instance;
    }

    // endregion
}
