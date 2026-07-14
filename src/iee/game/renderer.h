#pragma once
#include <cstdint>

#include "iee/core/config.h"

namespace iee::game {
struct BuildManifest;

struct TextureConfigurationStats {
  std::uint64_t calls{};
  std::uint64_t cacheHits{};
  std::uint64_t configured{};
  std::uint64_t latchedFailures{};
  std::uint64_t evictions{};
};

struct DrawApi {
  using DrawBegin_t = void (*)(int);
  using DrawEnd_t = void (*)();
  using DrawPushState_t = void (*)();
  using DrawPopState_t = void (*)();
  using DrawTexCoord_t = void (*)(int, int);
  using DrawVertex_t = void (*)(int, int);
  using DrawBindTexture_t = void (*)(int);
  using DrawDisable_t = void (*)(int);
  using DrawColor_t = unsigned long (*)(unsigned long);
  using DrawColorTone_t = void (*)(int);
  using CRes_Demand_t = void* (*)(void*);

  DrawBegin_t DrawBegin{};
  DrawEnd_t DrawEnd{};
  DrawPushState_t DrawPushState{};
  DrawPopState_t DrawPopState{};
  DrawTexCoord_t DrawTexCoord{};
  DrawVertex_t DrawVertex{};
  DrawBindTexture_t DrawBindTexture{};
  DrawDisable_t DrawDisable{};
  DrawColor_t DrawColor{};
  DrawColorTone_t DrawColorTone{};
  CRes_Demand_t CRes_Demand{};
};

bool resolve_draw_api(DrawApi& out, std::uintptr_t renderTextureVA, const BuildManifest& manifest);

// Configures the texture currently bound to GL_TEXTURE_2D. Returns false
// when no texture is bound or any requested parameter fails.
bool configure_bound_texture(const core::EngineConfig& cfg, int sourceTextureId);

// Requests a drop of the bounded per-area cache used by
// configure_bound_texture(). The render thread consumes the request; context
// recreation also invalidates the cache automatically.
void request_texture_configuration_cache_reset() noexcept;

// Monotonic invalidation epoch consumed by the tile renderer before applying
// its consecutive-texture fast path.
[[nodiscard]] std::uint64_t texture_configuration_epoch() noexcept;
[[nodiscard]] TextureConfigurationStats take_texture_configuration_stats() noexcept;

constexpr unsigned long BLACK_COLOR = 0xFF000000;
}  // namespace iee::game
