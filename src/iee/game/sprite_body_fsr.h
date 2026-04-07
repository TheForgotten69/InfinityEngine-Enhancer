#pragma once

#include "iee/core/config.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace iee::game::sprite_body_fsr {
    using GLenum = unsigned int;
    using GLuint = unsigned int;
    using GLint = int;
    using GLsizei = int;

    struct DrawCallInfo {
        GLuint program{};
        GLuint framebuffer{};
        GLint samplerUnit{-1};
        GLuint texture{};
    };

    using DrawArraysFn = void (APIENTRY*)(GLenum, GLint, GLsizei);
    using DrawElementsFn = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void *);

    bool install(const core::EngineConfig &cfg);

    bool matches_configured_target(GLuint program, GLuint texture, GLuint framebuffer) noexcept;

    bool is_internal_pass_active() noexcept;

    void on_external_bind_framebuffer(GLenum target, GLuint framebuffer);

    void reset_runtime_state(const char *reason = nullptr) noexcept;

    bool handle_draw_arrays(const DrawCallInfo &info,
                            GLenum mode,
                            GLint first,
                            GLsizei count,
                            DrawArraysFn original);

    bool handle_draw_elements(const DrawCallInfo &info,
                              GLenum mode,
                              GLsizei count,
                              GLenum type,
                              const void *indices,
                              DrawElementsFn original);

    void uninstall() noexcept;
}
