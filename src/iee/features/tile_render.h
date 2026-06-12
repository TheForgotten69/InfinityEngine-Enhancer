#pragma once
#include <atomic>
#include <cstdint>

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
        // Bumped on every reset to invalidate per-thread scale caches.
        std::atomic<std::uint32_t> areaId{0};

        void reset() noexcept {
            lastTexId.store(-1);
            areaScale.store(1);
            scaleDetected.store(false);
            detectionCount.store(0);
            areaId.fetch_add(1);
        }
    };

    TileRenderState &tile_render_state() noexcept;

    // Tile upscale render path. Returns true if it fully handled the draw;
    // false means the caller must invoke the original RenderTexture.
    // Never calls the original itself; hook enable/disable stays with the dispatcher,
    // which must check should_disable_render_hook() after each call.
    bool render_tile(AppContext &ctx, void *vidTile, int texId, void *unused,
                     int x, int y, unsigned long flags);

    bool should_disable_render_hook() noexcept;
    void clear_disable_request() noexcept;
}
