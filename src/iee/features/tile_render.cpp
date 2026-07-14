#include "tile_render.h"

#include <cstdint>
#include <optional>

#include "iee/app_context.h"
#include "iee/core/logger.h"
#include "iee/diagnostics.h"
#include "iee/game/game_types.h"
#include "iee/game/renderer.h"
#include "iee/game/tile_upscale.h"
#include "iee/game/tis_runtime.h"

namespace iee::features {
namespace {
std::atomic<bool> g_disableRenderHookRequest{false};

void request_render_hook_disable() noexcept {
  g_disableRenderHookRequest.store(true, std::memory_order_relaxed);
}
}  // namespace

TileRenderState& tile_render_state() noexcept {
  static TileRenderState state;
  return state;
}

bool should_disable_render_hook() noexcept {
  return g_disableRenderHookRequest.load(std::memory_order_relaxed);
}

void clear_disable_request() noexcept {
  g_disableRenderHookRequest.store(false, std::memory_order_relaxed);
}

bool render_tile(AppContext& ctx, void* vidTile, int texId, void* unused, int x, int y,
                 unsigned long flags) {
  (void)unused;

  using game::DrawMode;
  using game::ShaderTone;

  auto& state = tile_render_state();

  // Try to get tile information
  game::TileInfo tileInfo;
  if (!game::get_tile_info(vidTile, *ctx.manifest, tileInfo, ctx.draw.CRes_Demand)) {
    return false;
  }

  // Bounds check the tile index before accessing
  if (tileInfo.index < 0 || static_cast<std::uint32_t>(tileInfo.index) >= tileInfo.tileCount) {
    return false;
  }

  const auto& entry = tileInfo.entry;
  const int u0 = entry.u;
  const int v0 = entry.v;

  // Detect area scale from authored TIS metadata first, with heuristics as a fallback.
  if (!state.scaleDetected.load(std::memory_order_acquire)) {
    if (const auto detection = game::detect_scale(tileInfo, texId, *ctx.manifest)) {
      state.areaScale.store(detection->scaleFactor, std::memory_order_relaxed);
      state.scaleDetected.store(true, std::memory_order_release);

      switch (detection->source) {
        case game::ScaleDetectionSource::TisHeader:
          LOG_INFO("Detected {}x tiles from TIS header (tileDimension=0x{:X})",
                   detection->scaleFactor, detection->detectedTileDimension);
          break;
        case game::ScaleDetectionSource::TileTable:
          LOG_INFO("Detected {}x tiles from PVR entry table (grid step=0x{:X})",
                   detection->scaleFactor, detection->detectedTileDimension);
          break;
        case game::ScaleDetectionSource::Heuristic:
          LOG_INFO("Detected {}x tiles via heuristic fallback (texId={}, UV=({}, {}))",
                   detection->scaleFactor, texId, entry.u, entry.v);
          break;
      }

      if (detection->scaleFactor == 1) {
        request_render_hook_disable();
        return false;
      }
    } else if (state.detectionCount.load(std::memory_order_relaxed) <
               game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
      const int sampleCount = state.detectionCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (sampleCount == 1) {
        if (const auto headerTileDimension =
                game::get_tis_header_tile_dimension(tileInfo, *ctx.manifest)) {
          diagnostics::log_tis_header_diagnostics(
              vidTile, tileInfo,
              "Unsupported deterministic tile metadata; sampling before disabling",
              *headerTileDimension, true);
        } else {
          diagnostics::log_tis_header_diagnostics(
              vidTile, tileInfo,
              "TIS header missing and tile table did not resolve scale; sampling "
              "before disabling",
              std::nullopt, true);
        }
      }

      if (sampleCount == game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
        state.areaScale.store(1, std::memory_order_relaxed);
        state.scaleDetected.store(true, std::memory_order_release);
        request_render_hook_disable();
        LOG_INFO("Standard tiles detected via heuristic fallback after {} samples", sampleCount);
        return false;
      }
    } else {
      state.areaScale.store(1, std::memory_order_relaxed);
      state.scaleDetected.store(true, std::memory_order_release);
    }
  }

  const int scaleFactor = state.areaScale.load(std::memory_order_relaxed);
  const int du = game::TileDimensions::STANDARD_SIZE * scaleFactor;
  const int dv = game::TileDimensions::STANDARD_SIZE * scaleFactor;

  // Handle special texture cases
  unsigned long savedColor = 0;
  if (texId == 0) {
    if (ctx.draw.DrawDisable) ctx.draw.DrawDisable(1);
    if (ctx.draw.DrawColor) savedColor = ctx.draw.DrawColor(game::BLACK_COLOR);
  } else if (texId != -1) {
    if (ctx.draw.DrawBindTexture) ctx.draw.DrawBindTexture(texId);
  }

  // Apply texture enhancements to area tiles (detected by successful get_tile_info)
  // Only enhance textures that look like area tiles (higher IDs)
  // UI textures typically have lower IDs and should not be enhanced
  const int currentLastTex = state.lastTexId.load(std::memory_order_relaxed);
  if (texId != currentLastTex && texId > game::UpscaleThresholds::UI_TEXTURE_THRESHOLD) {
    const bool configured = game::configure_bound_texture(ctx.cfg, texId);
    state.lastTexId.store(texId, std::memory_order_relaxed);
    if (configured) {
      LOG_DEBUG_FAST("Enhanced tile texture {}", texId);
    } else {
      LOG_WARN("Failed to configure GL texture parameters for tile texture {}", texId);
    }
  }

  if (ctx.draw.DrawPushState) ctx.draw.DrawPushState();

  int tone = static_cast<int>(ShaderTone::Seam);

  // Check if grey tone is specifically requested via flags
  if (flags & game::RenderFlags::GREY_TONE_MASK) {
    tone = static_cast<int>(ShaderTone::Grey);
  }

  // Check the "linear tiles" switch in TIS structure
  if (game::get_tis_linear_tiles_flag(tileInfo.tileset, *ctx.manifest)) {
    tone = static_cast<int>(ShaderTone::Seam);
  }

  if (ctx.draw.DrawColorTone) ctx.draw.DrawColorTone(tone);

  if (ctx.draw.DrawBegin) ctx.draw.DrawBegin(static_cast<int>(DrawMode::Triangles));

  // Keep the 64×64 screen quad (lighting/scissor correctness)
  const int x0 = x;
  const int y0 = y;
  const int x1 = x + game::TileDimensions::RENDER_QUAD_SIZE;
  const int y1 = y + game::TileDimensions::RENDER_QUAD_SIZE;

  // Triangle 1
  if (ctx.draw.DrawTexCoord && ctx.draw.DrawVertex) {
    ctx.draw.DrawTexCoord(u0, v0);
    ctx.draw.DrawVertex(x0, y0);
    ctx.draw.DrawTexCoord(u0, v0 + dv);
    ctx.draw.DrawVertex(x0, y1);
    ctx.draw.DrawTexCoord(u0 + du, v0);
    ctx.draw.DrawVertex(x1, y0);

    // Triangle 2
    ctx.draw.DrawTexCoord(u0 + du, v0);
    ctx.draw.DrawVertex(x1, y0);
    ctx.draw.DrawTexCoord(u0, v0 + dv);
    ctx.draw.DrawVertex(x0, y1);
    ctx.draw.DrawTexCoord(u0 + du, v0 + dv);
    ctx.draw.DrawVertex(x1, y1);
  }

  if (ctx.draw.DrawEnd) ctx.draw.DrawEnd();
  if (texId == 0 && ctx.draw.DrawColor) ctx.draw.DrawColor(savedColor);
  if (ctx.draw.DrawPopState) ctx.draw.DrawPopState();

  return true;
}
}  // namespace iee::features
