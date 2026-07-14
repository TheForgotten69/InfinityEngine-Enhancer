#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace iee {
struct AppContext;
}

namespace iee::game {
struct CResTileSet;
}

namespace iee::features {
struct TilesetRenderState {
  const game::CResTileSet* tileset{};
  int scaleFactor{1};
  int detectionCount{};
  bool scaleDetected{};
  bool linearTiles{};
  bool linearFlagDetected{};
};

// Per-area tile upscale state, owned by this feature (moved out of AppContext).
struct TileRenderState {
  static constexpr std::size_t kMaxTilesetsPerArea = 16;

  std::atomic<int> lastTexId{-1};
  std::uint64_t lastTextureConfigurationEpoch{};
  std::array<TilesetRenderState, kMaxTilesetsPerArea> tilesets{};
  std::size_t tilesetCount{};
  int consecutiveDecodeFailures{};
  bool sawUpscaledTileset{};
  bool capacityWarningLogged{};

  TilesetRenderState* find_or_add(const game::CResTileSet* tileset) noexcept {
    for (std::size_t i = 0; i < tilesetCount; ++i) {
      if (tilesets[i].tileset == tileset) return &tilesets[i];
    }
    if (!tileset || tilesetCount >= tilesets.size()) return nullptr;
    auto& added = tilesets[tilesetCount++];
    added = {.tileset = tileset};
    return &added;
  }

  void reset() noexcept {
    lastTexId.store(-1, std::memory_order_relaxed);
    lastTextureConfigurationEpoch = 0;
    tilesets = {};
    tilesetCount = 0;
    consecutiveDecodeFailures = 0;
    sawUpscaledTileset = false;
    capacityWarningLogged = false;
  }
};

TileRenderState& tile_render_state() noexcept;

// LoadArea may run at a different engine boundary from rendering. Request a
// reset here; the render thread consumes it before touching non-atomic state.
void request_tile_render_state_reset() noexcept;

// Tile upscale render path. Returns true if it fully handled the draw;
// false means the caller must invoke the original RenderTexture.
// Never calls the original itself; the dispatcher remains installed for the
// area so modded overlays encountered after a standard base can still be
// classified independently.
bool render_tile(AppContext& ctx, void* vidTile, int texId, void* unused, int x, int y,
                 unsigned long flags);
}  // namespace iee::features
