#include "tile_render.h"

#include <cstdint>
#include <optional>

#include "iee/app_context.h"
#include "iee/diagnostics.h"
#include "iee/core/logger.h"
#include "iee/game/game_types.h"
#include "iee/game/renderer.h"
#include "iee/game/tile_upscale.h"
#include "iee/game/tis_runtime.h"

namespace iee::features {
    namespace {
        std::atomic<bool> g_disableRenderHookRequest{false};

        void request_render_hook_disable() noexcept {
            g_disableRenderHookRequest.store(true);
        }
    }

    TileRenderState &tile_render_state() noexcept {
        static TileRenderState state;
        return state;
    }

    bool should_disable_render_hook() noexcept {
        return g_disableRenderHookRequest.load();
    }

    void clear_disable_request() noexcept {
        g_disableRenderHookRequest.store(false);
    }

    bool render_tile(AppContext &ctx, void *vidTile, int texId, void *unused,
                     int x, int y, unsigned long flags) {
        (void) unused;

        using game::DrawMode;
        using game::ShaderTone;

        auto &state = tile_render_state();

        // Try to get tile information
        game::TileInfo tileInfo;
        if (!game::get_tile_info(vidTile, *ctx.manifest, tileInfo, ctx.draw.CRes_Demand)) {
            return false;
        }

        // Bounds check the tile index before accessing
        if (tileInfo.index < 0) {
            return false;
        }

        const auto &entry = tileInfo.table[tileInfo.index];
        int U0 = entry.u, V0 = entry.v;

        // Detect area scale from authored TIS metadata first, with heuristics as a fallback.
        if (!state.scaleDetected.load()) {
            if (const auto headerDetection = game::detect_scale_from_tis_header(tileInfo, *ctx.manifest)) {
                state.areaScale.store(headerDetection->scaleFactor);
                state.scaleDetected.store(true);

                if (headerDetection->scaleFactor > 1) {
                    LOG_INFO("Detected {}x tiles from TIS header (tileDimension=0x{:X})",
                             headerDetection->scaleFactor,
                             headerDetection->detectedTileDimension);
                } else {
                    request_render_hook_disable();
                    LOG_INFO("Detected standard tiles from TIS header (tileDimension=0x{:X})",
                             headerDetection->detectedTileDimension);
                    return false;
                }
            } else if (const auto tableDetection = game::detect_scale_from_tile_table(tileInfo)) {
                state.areaScale.store(tableDetection->scaleFactor);
                state.scaleDetected.store(true);

                if (tableDetection->scaleFactor > 1) {
                    LOG_INFO("Detected {}x tiles from PVR entry table (smallest step=0x{:X})",
                             tableDetection->scaleFactor,
                             tableDetection->detectedTileDimension);
                } else {
                    request_render_hook_disable();
                    LOG_INFO("Detected standard tiles from PVR entry table (smallest step=0x{:X})",
                             tableDetection->detectedTileDimension);
                    return false;
                }
            } else if (state.detectionCount.load() < game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
                const int sampleCount = state.detectionCount.fetch_add(1) + 1;
                if (sampleCount == 1) {
                    if (const auto headerTileDimension = game::get_tis_header_tile_dimension(tileInfo, *ctx.manifest)) {
                        diagnostics::log_tis_header_diagnostics(vidTile,
                                                                tileInfo,
                                                                "Unsupported deterministic tile metadata; using heuristic fallback",
                                                                *headerTileDimension,
                                                                true);
                    } else {
                        diagnostics::log_tis_header_diagnostics(vidTile,
                                                                tileInfo,
                                                                "TIS header missing and tile table did not resolve scale; using heuristic fallback",
                                                                std::nullopt,
                                                                true);
                    }
                }

                if (game::is_upscaled_by_heuristics(tileInfo, texId)) {
                    state.areaScale.store(4);
                    state.scaleDetected.store(true);
                    LOG_INFO("Upscaled tiles detected via heuristic fallback (sample {}, texId: {}, UV: ({},{}))",
                             sampleCount,
                             texId,
                             entry.u,
                             entry.v);
                } else if (sampleCount == game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
                    state.areaScale.store(1);
                    state.scaleDetected.store(true);
                    request_render_hook_disable();
                    LOG_INFO("Standard tiles detected via heuristic fallback after {} samples", sampleCount);
                    return false;
                }
            } else {
                state.areaScale.store(1);
                state.scaleDetected.store(true);
            }
        }

        int scaleFactor;
        if (state.scaleDetected.load()) {
            // Scale is stable after detection - use cached value with area change detection
            thread_local int cachedScale = 0;
            thread_local std::uint32_t cachedAreaId = 0;

            const std::uint32_t currentAreaId = state.areaId.load();
            if (cachedAreaId != currentAreaId) {
                cachedScale = state.areaScale.load();
                cachedAreaId = currentAreaId;
            }
            scaleFactor = cachedScale;
        } else {
            scaleFactor = state.areaScale.load();
        }
        int DU = game::TileDimensions::STANDARD_SIZE * scaleFactor, DV =
                game::TileDimensions::STANDARD_SIZE * scaleFactor;

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
        int currentLastTex = state.lastTexId.load();
        if (texId != currentLastTex && texId > game::UpscaleThresholds::UI_TEXTURE_THRESHOLD) {
            if (game::ensure_texture_params(ctx.cfg, ctx.draw, texId)) {
                state.lastTexId.store(texId);
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
        const int X0 = x, Y0 = y;
        const int X1 = x + game::TileDimensions::RENDER_QUAD_SIZE, Y1 = y + game::TileDimensions::RENDER_QUAD_SIZE;

        // Triangle 1
        if (ctx.draw.DrawTexCoord && ctx.draw.DrawVertex) {
            ctx.draw.DrawTexCoord(U0, V0);
            ctx.draw.DrawVertex(X0, Y0);
            ctx.draw.DrawTexCoord(U0, V0 + DV);
            ctx.draw.DrawVertex(X0, Y1);
            ctx.draw.DrawTexCoord(U0 + DU, V0);
            ctx.draw.DrawVertex(X1, Y0);

            // Triangle 2
            ctx.draw.DrawTexCoord(U0 + DU, V0);
            ctx.draw.DrawVertex(X1, Y0);
            ctx.draw.DrawTexCoord(U0, V0 + DV);
            ctx.draw.DrawVertex(X0, Y1);
            ctx.draw.DrawTexCoord(U0 + DU, V0 + DV);
            ctx.draw.DrawVertex(X1, Y1);
        }

        if (ctx.draw.DrawEnd) ctx.draw.DrawEnd();
        if (texId == 0 && ctx.draw.DrawColor) ctx.draw.DrawColor(savedColor);
        if (ctx.draw.DrawPopState) ctx.draw.DrawPopState();

        return true;
    }
}
