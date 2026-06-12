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
