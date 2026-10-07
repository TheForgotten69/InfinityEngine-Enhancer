#pragma once

#include "iee/game/opengl_types.h"

namespace iee::core {
// Saves the GL state our own draw passes change and puts it back on scope
// exit. The engine issues state changes relative to what it believes the
// hardware state is, so anything left altered corrupts its next draws.
// Covers program, vertex array, viewport, blending and the per-fragment tests.
// Framebuffer and texture bindings are not covered: the caller owns the
// framebuffer hand-over, and GlStateGuard covers texture units.
// Requires OpenGLFunctions::postProcessAvailable.
class GlPassGuard {
 public:
  GlPassGuard() noexcept {
    const auto& gl = game::gl::get_gl_functions();
    gl.glGetIntegerv(game::gl::CURRENT_PROGRAM, &program_);
    gl.glGetIntegerv(game::gl::VERTEX_ARRAY_BINDING, &vertexArray_);
    gl.glGetIntegerv(game::gl::ARRAY_BUFFER_BINDING, &arrayBuffer_);
    gl.glGetIntegerv(game::gl::VIEWPORT, viewport_);
    gl.glGetIntegerv(game::gl::BLEND_SRC_RGB, &blendSrcRgb_);
    gl.glGetIntegerv(game::gl::BLEND_DST_RGB, &blendDstRgb_);
    gl.glGetIntegerv(game::gl::BLEND_SRC_ALPHA, &blendSrcAlpha_);
    gl.glGetIntegerv(game::gl::BLEND_DST_ALPHA, &blendDstAlpha_);
    gl.glGetFloatv(game::gl::COLOR_CLEAR_VALUE, clearColor_);
    blend_ = gl.glIsEnabled(game::gl::BLEND) != 0;
    scissor_ = gl.glIsEnabled(game::gl::SCISSOR_TEST) != 0;
    depthTest_ = gl.glIsEnabled(game::gl::DEPTH_TEST) != 0;
    cullFace_ = gl.glIsEnabled(game::gl::CULL_FACE) != 0;
    stencilTest_ = gl.glIsEnabled(game::gl::STENCIL_TEST) != 0;
    alphaTest_ = gl.glIsEnabled(game::gl::ALPHA_TEST) != 0;
  }

  ~GlPassGuard() noexcept {
    const auto& gl = game::gl::get_gl_functions();
    const auto set = [&](unsigned capability, bool enabled) {
      if (enabled) {
        gl.glEnable(capability);
      } else {
        gl.glDisable(capability);
      }
    };
    set(game::gl::BLEND, blend_);
    set(game::gl::SCISSOR_TEST, scissor_);
    set(game::gl::DEPTH_TEST, depthTest_);
    set(game::gl::CULL_FACE, cullFace_);
    set(game::gl::STENCIL_TEST, stencilTest_);
    set(game::gl::ALPHA_TEST, alphaTest_);
    gl.glBlendFuncSeparate(
        static_cast<unsigned>(blendSrcRgb_), static_cast<unsigned>(blendDstRgb_),
        static_cast<unsigned>(blendSrcAlpha_), static_cast<unsigned>(blendDstAlpha_));
    gl.glClearColor(clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]);
    gl.glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
    gl.glBindVertexArray(static_cast<unsigned>(vertexArray_));
    gl.glBindBuffer(game::gl::ARRAY_BUFFER, static_cast<unsigned>(arrayBuffer_));
    gl.glUseProgram(static_cast<unsigned>(program_));
  }

  GlPassGuard(const GlPassGuard&) = delete;
  GlPassGuard& operator=(const GlPassGuard&) = delete;

  // The viewport as the engine left it: {x, y, width, height}.
  [[nodiscard]] const int* viewport() const noexcept { return viewport_; }

 private:
  int program_{};
  int vertexArray_{};
  int arrayBuffer_{};
  int viewport_[4]{};
  int blendSrcRgb_{};
  int blendDstRgb_{};
  int blendSrcAlpha_{};
  int blendDstAlpha_{};
  float clearColor_[4]{};
  bool blend_{};
  bool scissor_{};
  bool depthTest_{};
  bool cullFace_{};
  bool stencilTest_{};
  bool alphaTest_{};
};
}  // namespace iee::core
