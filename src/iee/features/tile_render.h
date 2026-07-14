#pragma once
#include <atomic>

namespace iee {
struct AppContext;
}

namespace iee::features {
// Per-area tile upscale state, owned by this feature (moved out of AppContext).
struct TileRenderState {
  std::atomic<int> lastTexId{-1};
  std::atomic<int> areaScale{1};
  std::atomic<bool> scaleDetected{false};
  std::atomic<int> detectionCount{0};
  void reset() noexcept {
    lastTexId.store(-1, std::memory_order_relaxed);
    areaScale.store(1, std::memory_order_relaxed);
    detectionCount.store(0, std::memory_order_relaxed);
    scaleDetected.store(false, std::memory_order_release);
  }
};

TileRenderState& tile_render_state() noexcept;

// Tile upscale render path. Returns true if it fully handled the draw;
// false means the caller must invoke the original RenderTexture.
// Never calls the original itself; hook enable/disable stays with the dispatcher,
// which must check should_disable_render_hook() after each call.
bool render_tile(AppContext& ctx, void* vidTile, int texId, void* unused, int x, int y,
                 unsigned long flags);

bool should_disable_render_hook() noexcept;
void clear_disable_request() noexcept;
}  // namespace iee::features
