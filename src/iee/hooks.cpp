#include "hooks.h"
#include "app_context.h"
#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/game/game_types.h"
#include "iee/game/renderer.h"
#include <windows.h>

namespace iee::hooks {
    // region Hook function type definitions
    using Fn_LoadArea = void* (*)(void *, void *, unsigned char, unsigned char, unsigned char);
    using Fn_RenderTexture = void (*)(void *, int, void *, int, int, unsigned long);
    using Fn_DrawColorTone = void (*)(int);

    // Hook management - initialize MinHook
    static core::HookInit hkInit;  // Required for MinHook initialization
    static core::Hook<Fn_LoadArea> H_LoadArea;
    static core::Hook<Fn_RenderTexture> H_RenderTexture;
    static core::Hook<Fn_DrawColorTone> H_DrawColorTone;

    static AppContext *g_ctx = nullptr;

    static std::atomic<uint32_t> g_currentAreaId{0};

    using game::DrawMode;
    using game::ShaderTone;


    // Function to trigger cache reset from LoadArea
    static void reset_thread_local_cache() {
        g_currentAreaId.fetch_add(1); // Increment to invalidate all thread-local caches
    }

    // LoadArea hook - reset area-specific state for new area detection
    static void *Detour_LoadArea(void *thisPtr, void *pAreaNameString, unsigned char a2, unsigned char a3,
                                 unsigned char a4) {
        LOG_DEBUG("LoadArea called - resetting scale detection for new area");

        auto &ctx = *g_ctx;
        ctx.reset_area_state();

        // Reset thread-local cache for new area
        reset_thread_local_cache();

        // Re-enable RenderTexture hook for new area detection
        if (!ctx.isRenderHookActive) {
            try {
                H_RenderTexture.enable();
                ctx.isRenderHookActive = true;
                LOG_DEBUG("Re-enabled RenderTexture hook for new area detection");
            } catch (const std::exception &e) {
                LOG_WARN("Failed to re-enable RenderTexture hook: {}", e.what());
            }
        }

        return H_LoadArea.original()(thisPtr, pAreaNameString, a2, a3, a4);
    }

    // DrawColorTone hook - simple passthrough with tracking
    static void Detour_DrawColorTone(int mode) {
        H_DrawColorTone.original()(mode);
    }

    // Main RenderTexture hook - handles HD detection and texture enhancement
    static void Detour_RenderTexture(void *thisPtr, int texId, void *unused, int x, int y, unsigned long flags) {
        // Safety check - ensure context is valid
        if (!g_ctx) {
            return; // Context destroyed, don't process
        }

        auto &ctx = *g_ctx;

        // Try to get tile information
        game::TileInfo tileInfo;
        if (!game::get_tile_info(thisPtr, tileInfo, ctx.draw.CRes_Demand)) {
            // Can't resolve tile info - use original function
            H_RenderTexture.original()(thisPtr, texId, unused, x, y, flags);
            return;
        }

        // Bounds check the tile index before accessing
        if (tileInfo.index < 0) {
            H_RenderTexture.original()(thisPtr, texId, unused, x, y, flags);
            return;
        }

        const auto &entry = tileInfo.table[tileInfo.index];
        int U0 = entry.u, V0 = entry.v;

        // Detect upscaled content and update area-wide scale factor
        if (!ctx.scaleDetected.load() && ctx.detectionCount.load() < game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
            ctx.detectionCount.fetch_add(1);

            if (entry.u > game::UpscaleThresholds::UV_THRESHOLD || entry.v > game::UpscaleThresholds::UV_THRESHOLD ||
                texId > game::UpscaleThresholds::TEXTURE_ID_THRESHOLD) {
                // Upscaled content detected - set area-wide 4x scaling
                ctx.areaScale.store(4);
                ctx.scaleDetected.store(true);
                LOG_INFO(
                    "Upscaled tiles detected! Setting the new scale factor for entire area (texId: {}, UV: ({},{}))",
                    texId, entry.u, entry.v);
            } else if (ctx.detectionCount.load() == game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
                ctx.areaScale.store(1);
                ctx.scaleDetected.store(true);

                try {
                    H_RenderTexture.disable();
                    ctx.isRenderHookActive = false;
                    LOG_INFO("Standard area detected");
                } catch (const std::exception &e) {
                    LOG_WARN("Failed to disable RenderTexture hook: {}", e.what());
                }
            }
        }

        int scaleFactor;
        if (ctx.scaleDetected.load()) {
            // Scale is stable after detection - use cached value with area change detection
            thread_local int cachedScale = 0;
            thread_local uint32_t cachedAreaId = 0;

            uint32_t currentAreaId = g_currentAreaId.load();
            if (cachedAreaId != currentAreaId) {
                cachedScale = ctx.areaScale.load();
                cachedAreaId = currentAreaId;
            }
            scaleFactor = cachedScale;
        } else {
            scaleFactor = ctx.areaScale.load();
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
        int currentLastTex = ctx.lastTexId.load();
        if (texId != currentLastTex && texId > game::UpscaleThresholds::UI_TEXTURE_THRESHOLD) {
            if (game::ensure_texture_params(ctx.cfg, ctx.draw, texId)) {
                ctx.lastTexId.store(texId);
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
        if (game::get_tis_linear_tiles_flag(tileInfo.tis)) {
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
    }


    bool install_all(AppContext &ctx) {
        g_ctx = &ctx;

        try {
            H_LoadArea.create(reinterpret_cast<void *>(ctx.addrs.LoadArea), (void *) &Detour_LoadArea);
            LOG_INFO("LoadArea hook created");

            H_RenderTexture.create(reinterpret_cast<void *>(ctx.addrs.RenderTexture), (void *) &Detour_RenderTexture);
            LOG_INFO("RenderTexture hook created");

            if (ctx.draw.DrawColorTone) {
                try {
                    H_DrawColorTone.create(reinterpret_cast<void *>(ctx.draw.DrawColorTone),
                                           (void *) &Detour_DrawColorTone);
                    H_DrawColorTone.enable();
                    LOG_INFO("DrawColorTone hook installed");
                } catch (const std::exception &e) {
                    LOG_WARN("Failed to create DrawColorTone hook: {} - continuing without it", e.what());
                }
            }

            H_LoadArea.enable();
            LOG_INFO("LoadArea hook enabled");

            H_RenderTexture.enable();
            LOG_INFO("RenderTexture hook enabled");

            ctx.isRenderHookActive = true;

            LOG_INFO("All hooks installed successfully");
            LOG_INFO("LoadArea: 0x{:X}", ctx.addrs.LoadArea);
            LOG_INFO("RenderTexture: 0x{:X}", ctx.addrs.RenderTexture);
            LOG_INFO("RenderTexture hook enabled - will detect upscaled textures automatically");

            return true;
        } catch (const std::exception &e) {
            LOG_ERROR("Exception during hook installation: {}", e.what());
            return false;
        } catch (...) {
            LOG_ERROR("Unknown exception during hook installation");
            return false;
        }
    }

    void uninstall_all() {
        LOG_INFO("Uninstalling all hooks...");

        H_DrawColorTone.disable();
        H_RenderTexture.disable();
        H_LoadArea.disable();

        g_ctx = nullptr;

        LOG_INFO("Hook cleanup complete");
    }

    bool is_active() {
        return g_ctx != nullptr && g_ctx->isRenderHookActive;
    }
}
