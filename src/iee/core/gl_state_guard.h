#pragma once
#include <array>
#include <initializer_list>

#include "iee/game/opengl_types.h"

namespace iee::core {
    // Saves the GL state touched by IEE upload operations. Texture units are
    // explicit so uploads can restore the engine's bindings while draw-time
    // binding remains controlled by the uniform bridge.
    class GlStateGuard {
      public:
        explicit GlStateGuard(std::initializer_list<unsigned> textureUnits = {}) noexcept {
            auto &gl = game::gl::get_gl_functions();
            if (gl.glGetIntegerv) {
                gl.glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &activeTexture_);
                gl.glGetIntegerv(game::gl::UNPACK_ALIGNMENT, &unpackAlignment_);
            }
            if (gl.glActiveTexture && gl.glGetIntegerv) {
                for (const auto unit : textureUnits) {
                    if (bindingCount_ >= bindings_.size()) break;
                    auto &binding = bindings_[bindingCount_++];
                    binding.unit = unit;
                    gl.glActiveTexture(game::gl::TEXTURE0 + unit);
                    gl.glGetIntegerv(game::gl::TEXTURE_BINDING_2D, &binding.texture);
                }
            }
        }
        ~GlStateGuard() noexcept {
            auto &gl = game::gl::get_gl_functions();
            if (gl.glActiveTexture && gl.glBindTexture) {
                for (std::size_t index = 0; index < bindingCount_; ++index) {
                    const auto &binding = bindings_[index];
                    gl.glActiveTexture(game::gl::TEXTURE0 + binding.unit);
                    gl.glBindTexture(game::gl::TEXTURE_2D, static_cast<unsigned>(binding.texture));
                }
            }
            if (gl.glPixelStorei && unpackAlignment_ > 0) {
                gl.glPixelStorei(game::gl::UNPACK_ALIGNMENT, unpackAlignment_);
            }
            if (gl.glActiveTexture && activeTexture_ >= 0) gl.glActiveTexture(static_cast<unsigned>(activeTexture_));
        }
        GlStateGuard(const GlStateGuard &) = delete;
        GlStateGuard &operator=(const GlStateGuard &) = delete;

      private:
        struct TextureBinding {
            unsigned unit{};
            int texture{};
        };

        int activeTexture_{-1};
        int unpackAlignment_{-1};
        std::array<TextureBinding, 8> bindings_{};
        std::size_t bindingCount_{};
    };
} // namespace iee::core
