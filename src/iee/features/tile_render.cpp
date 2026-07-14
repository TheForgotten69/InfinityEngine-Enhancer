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
std::atomic<bool> g_resetRenderStateRequest{false};

void request_render_hook_disable() noexcept {
  g_disableRenderHookRequest.store(true, std::memory_order_relaxed);
}
}  // namespace

TileRenderState& tile_render_state() noexcept {
  static TileRenderState state;
  return state;
}

void request_tile_render_state_reset() noexcept {
  g_resetRenderStateRequest.store(true, std::memory_order_release);
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
  if (g_resetRenderStateRequest.exchange(false, std::memory_order_acquire)) {
    state.reset();
  }

  // Try to get tile information
  game::TileInfo tileInfo;
  if (!game::get_tile_info(vidTile, *ctx.manifest, tileInfo, ctx.draw.CRes_Demand)) {
    state.lastTexId.store(-1, std::memory_order_relaxed);
    ++state.consecutiveDecodeFailures;
    if (!state.sawUpscaledTileset &&
        state.consecutiveDecodeFailures >= game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
      request_render_hook_disable();
      if (state.consecutiveDecodeFailures == game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
        LOG_WARN(
            "RenderTexture hook could not decode {} consecutive tile resources; disabling tile "
            "upscaling for this area while retaining the engine renderer",
            state.consecutiveDecodeFailures);
      }
    }
    return false;
  }
  state.consecutiveDecodeFailures = 0;

  // Bounds check the tile index before accessing
  if (tileInfo.index < 0 || static_cast<std::uint32_t>(tileInfo.index) >= tileInfo.tileCount) {
    state.lastTexId.store(-1, std::memory_order_relaxed);
    return false;
  }

  const auto& entry = tileInfo.entry;
  const int u0 = entry.u;
  const int v0 = entry.v;

  auto* tilesetState = state.find_or_add(tileInfo.tileset);
  if (!tilesetState) {
    state.lastTexId.store(-1, std::memory_order_relaxed);
    if (!state.capacityWarningLogged) {
      state.capacityWarningLogged = true;
      LOG_WARN(
          "Area exceeded the bounded {}-tileset runtime cache; delegating uncached tiles to the "
          "engine renderer",
          TileRenderState::kMaxTilesetsPerArea);
    }
    return false;
  }

  // Detect scale independently for each observed tileset. Standard-only areas
  // keep the existing fast-disable path; standard overlays discovered after a
  // 4x base tileset delegate to the engine without inheriting the 4x scale.
  if (!tilesetState->scaleDetected) {
    if (const auto detection = game::detect_scale(tileInfo, texId, *ctx.manifest)) {
      tilesetState->scaleFactor = detection->scaleFactor;
      tilesetState->scaleDetected = true;
      if (detection->scaleFactor > 1) state.sawUpscaledTileset = true;

      switch (detection->source) {
        case game::ScaleDetectionSource::TisHeader:
          LOG_INFO("Detected {}x tileset 0x{:X} from TIS header (tileDimension=0x{:X})",
                   detection->scaleFactor, reinterpret_cast<std::uintptr_t>(tileInfo.tileset),
                   detection->detectedTileDimension);
          break;
        case game::ScaleDetectionSource::TileTable:
          LOG_INFO("Detected {}x tileset 0x{:X} from PVR entry table (grid step=0x{:X})",
                   detection->scaleFactor, reinterpret_cast<std::uintptr_t>(tileInfo.tileset),
                   detection->detectedTileDimension);
          break;
        case game::ScaleDetectionSource::Heuristic:
          LOG_INFO("Detected {}x tileset 0x{:X} via heuristic fallback (texId={}, UV=({}, {}))",
                   detection->scaleFactor, reinterpret_cast<std::uintptr_t>(tileInfo.tileset),
                   texId, entry.u, entry.v);
          break;
      }

      if (detection->scaleFactor == 1) {
        state.lastTexId.store(-1, std::memory_order_relaxed);
        if (!state.sawUpscaledTileset) request_render_hook_disable();
        return false;
      }
    } else if (tilesetState->detectionCount < game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
      const int sampleCount = ++tilesetState->detectionCount;
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
        tilesetState->scaleFactor = 1;
        tilesetState->scaleDetected = true;
        if (!state.sawUpscaledTileset) request_render_hook_disable();
        LOG_INFO("Tileset 0x{:X} delegated as standard after {} inconclusive samples",
                 reinterpret_cast<std::uintptr_t>(tileInfo.tileset), sampleCount);
      }
      state.lastTexId.store(-1, std::memory_order_relaxed);
      return false;
    }
  }

  if (tilesetState->scaleFactor <= 1) {
    state.lastTexId.store(-1, std::memory_order_relaxed);
    return false;
  }

  const int scaleFactor = tilesetState->scaleFactor;
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

  // Successful tile decoding is the identity check; an arbitrary texture-ID
  // threshold can reject valid area textures. The renderer keeps a bounded
  // cache of actual GL texture names and validates recycled names.
  const int currentLastTex = state.lastTexId.load(std::memory_order_relaxed);
  if (texId != currentLastTex && texId > 0) {
    const bool configured = game::configure_bound_texture(ctx.cfg, texId);
    if (configured) {
      state.lastTexId.store(texId, std::memory_order_relaxed);
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
  if (!tilesetState->linearFlagDetected) {
    tilesetState->linearTiles = game::get_tis_linear_tiles_flag(tileInfo.tileset, *ctx.manifest);
    tilesetState->linearFlagDetected = true;
  }
  if (tilesetState->linearTiles) {
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
