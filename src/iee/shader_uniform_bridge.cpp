#include "shader_uniform_bridge.h"

#include <algorithm>
#include <atomic>

#include "area_state.h"
#include "iee/game/opengl_types.h"
#include "iee/water_textures.h"

namespace iee::probe::uniforms {
namespace {

std::atomic<float> g_time{0.0f};
std::atomic<float> g_worldWidth{0.0f};
std::atomic<float> g_worldHeight{0.0f};
std::atomic<float> g_scrollX{0.0f};
std::atomic<float> g_scrollY{0.0f};
std::atomic<float> g_viewWorldWidth{0.0f};
std::atomic<float> g_viewWorldHeight{0.0f};
std::atomic<float> g_waterTintR{0.5f};
std::atomic<float> g_waterTintG{0.5f};
std::atomic<float> g_waterTintB{0.5f};
std::atomic<float> g_effectValue{0.0f};
std::atomic<unsigned> g_feedCount{0};

int resolve_location(const game::gl::OpenGLFunctions& gl, unsigned program, int current,
                     const char* name) {
  if (current != Locations::kUnresolved) {
    return current;
  }
  return gl.glGetUniformLocation(program, name);
}

}  // namespace

void initialize(bool effectEnabled) noexcept {
  g_effectValue.store(effectEnabled ? 1.0f : 0.0f, std::memory_order_relaxed);
  g_feedCount.store(0, std::memory_order_relaxed);
}

void reset() noexcept {
  g_time.store(0.0f, std::memory_order_relaxed);
  g_worldWidth.store(0.0f, std::memory_order_relaxed);
  g_worldHeight.store(0.0f, std::memory_order_relaxed);
  g_scrollX.store(0.0f, std::memory_order_relaxed);
  g_scrollY.store(0.0f, std::memory_order_relaxed);
  g_viewWorldWidth.store(0.0f, std::memory_order_relaxed);
  g_viewWorldHeight.store(0.0f, std::memory_order_relaxed);
  g_waterTintR.store(0.5f, std::memory_order_relaxed);
  g_waterTintG.store(0.5f, std::memory_order_relaxed);
  g_waterTintB.store(0.5f, std::memory_order_relaxed);
  g_effectValue.store(0.0f, std::memory_order_relaxed);
  g_feedCount.store(0, std::memory_order_relaxed);
}

void set_time(float secondsSinceStart) noexcept {
  g_time.store(secondsSinceStart, std::memory_order_relaxed);
}

void set_effect_enabled(bool enabled) noexcept {
  g_effectValue.store(enabled ? 1.0f : 0.0f, std::memory_order_relaxed);
}

bool effect_enabled() noexcept { return g_effectValue.load(std::memory_order_relaxed) >= 0.5f; }

float cycle_debug_effect() noexcept {
  const float current = g_effectValue.load(std::memory_order_relaxed);
  const float next = current < 0.5f ? 1.0f : (current < 1.5f ? 2.0f : 0.0f);
  g_effectValue.store(next, std::memory_order_relaxed);
  return next;
}

void set_world_size(float widthPx, float heightPx) noexcept {
  g_worldWidth.store(std::max(widthPx, 0.0f), std::memory_order_relaxed);
  g_worldHeight.store(std::max(heightPx, 0.0f), std::memory_order_relaxed);
}

void set_water_tint(float r, float g, float b) noexcept {
  g_waterTintR.store(r, std::memory_order_relaxed);
  g_waterTintG.store(g, std::memory_order_relaxed);
  g_waterTintB.store(b, std::memory_order_relaxed);
}

void set_view(float scrollX, float scrollY, float viewWorldWidth, float viewWorldHeight) noexcept {
  g_scrollX.store(scrollX, std::memory_order_relaxed);
  g_scrollY.store(scrollY, std::memory_order_relaxed);
  g_viewWorldWidth.store(std::max(viewWorldWidth, 0.0f), std::memory_order_relaxed);
  g_viewWorldHeight.store(std::max(viewWorldHeight, 0.0f), std::memory_order_relaxed);
}

Snapshot snapshot() noexcept {
  return {
      .effectValue = g_effectValue.load(std::memory_order_relaxed),
      .scrollX = g_scrollX.load(std::memory_order_relaxed),
      .scrollY = g_scrollY.load(std::memory_order_relaxed),
      .viewWorldWidth = g_viewWorldWidth.load(std::memory_order_relaxed),
      .viewWorldHeight = g_viewWorldHeight.load(std::memory_order_relaxed),
      .worldWidth = g_worldWidth.load(std::memory_order_relaxed),
      .worldHeight = g_worldHeight.load(std::memory_order_relaxed),
      .feedCount = g_feedCount.load(std::memory_order_relaxed),
  };
}

void feed(unsigned program, Locations& locations) {
  const auto& gl = game::gl::get_gl_functions();
  if (!gl.glGetUniformLocation || !gl.glUniform1f) {
    return;
  }

  locations.time = resolve_location(gl, program, locations.time, "uIeeTime");
  locations.enabled = resolve_location(gl, program, locations.enabled, "uIeeEnabled");
  locations.scroll = resolve_location(gl, program, locations.scroll, "uIeeScroll");
  locations.zoom = resolve_location(gl, program, locations.zoom, "uIeeZoom");
  locations.viewport = resolve_location(gl, program, locations.viewport, "uIeeViewport");
  locations.worldSizeInv =
      resolve_location(gl, program, locations.worldSizeInv, "uIeeWorldSizeInv");
  locations.waterTint = resolve_location(gl, program, locations.waterTint, "uIeeWaterTint");
  locations.areaMask = resolve_location(gl, program, locations.areaMask, "uIeeAreaMask");
  locations.normalMap = resolve_location(gl, program, locations.normalMap, "uIeeNormalMap");
  locations.dudvMap = resolve_location(gl, program, locations.dudvMap, "uIeeDudvMap");
  locations.foamMap = resolve_location(gl, program, locations.foamMap, "uIeeFoamMap");

  g_feedCount.fetch_add(1, std::memory_order_relaxed);
  float effectValue = g_effectValue.load(std::memory_order_relaxed);
  if (locations.areaMask >= 0 && !area::bind_area_texture()) {
    effectValue = 0.0f;
  }
  if ((locations.normalMap >= 0 || locations.dudvMap >= 0 || locations.foamMap >= 0) &&
      !water::ensure_water_textures_bound()) {
    effectValue = 0.0f;
  }

  if (locations.time >= 0) {
    gl.glUniform1f(locations.time, g_time.load(std::memory_order_relaxed));
  }
  if (locations.enabled >= 0) {
    gl.glUniform1f(locations.enabled, effectValue);
  }
  if (locations.scroll >= 0 && gl.glUniform2f) {
    gl.glUniform2f(locations.scroll, g_scrollX.load(std::memory_order_relaxed),
                   g_scrollY.load(std::memory_order_relaxed));
  }

  int viewport[4] = {0, 0, 0, 0};
  if (gl.glGetIntegerv) {
    gl.glGetIntegerv(0x0BA2 /* GL_VIEWPORT */, viewport);
  }
  const float viewWidth = g_viewWorldWidth.load(std::memory_order_relaxed);
  const float viewHeight = g_viewWorldHeight.load(std::memory_order_relaxed);
  if (locations.zoom >= 0 && gl.glUniform2f && viewWidth > 0.0f && viewHeight > 0.0f &&
      viewport[2] > 0 && viewport[3] > 0) {
    gl.glUniform2f(locations.zoom, static_cast<float>(viewport[2]) / viewWidth,
                   static_cast<float>(viewport[3]) / viewHeight);
  }
  if (locations.viewport >= 0 && gl.glUniform2f) {
    gl.glUniform2f(locations.viewport, static_cast<float>(viewport[2]),
                   static_cast<float>(viewport[3]));
  }

  const float worldWidth = g_worldWidth.load(std::memory_order_relaxed);
  const float worldHeight = g_worldHeight.load(std::memory_order_relaxed);
  if (locations.worldSizeInv >= 0 && gl.glUniform2f && worldWidth > 0.0f && worldHeight > 0.0f) {
    gl.glUniform2f(locations.worldSizeInv, 1.0f / worldWidth, 1.0f / worldHeight);
  }
  if (locations.waterTint >= 0 && gl.glUniform3f) {
    gl.glUniform3f(locations.waterTint, g_waterTintR.load(std::memory_order_relaxed),
                   g_waterTintG.load(std::memory_order_relaxed),
                   g_waterTintB.load(std::memory_order_relaxed));
  }
  if (locations.areaMask >= 0 && gl.glUniform1i) {
    gl.glUniform1i(locations.areaMask, 2);
  }
  if (locations.normalMap >= 0 && gl.glUniform1i) {
    gl.glUniform1i(locations.normalMap, 3);
  }
  if (locations.dudvMap >= 0 && gl.glUniform1i) {
    gl.glUniform1i(locations.dudvMap, 4);
  }
  if (locations.foamMap >= 0 && gl.glUniform1i) {
    gl.glUniform1i(locations.foamMap, 5);
  }
}

}  // namespace iee::probe::uniforms
