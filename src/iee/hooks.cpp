#include "hooks.h"

#include <atomic>
#include <windows.h>

#include "app_context.h"
#include "area_state.h"
#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/features/tile_render.h"
#include "iee/shader_probe.h"

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

    namespace {
        void install_shader_probes_once() {
            // Latch only on success: a transient first-frame failure (partial GL
            // table) must not permanently suppress the probes. Runs on the render
            // thread only, so plain statics are safe.
            static bool installed = false;
            static bool warnedOnce = false;
            if (installed) {
                return;
            }
            if (!warnedOnce) {
                probe::log_shader_runtime_capabilities();
            }
            if (probe::install_shader_probes(g_ctx->cfg)) {
                installed = true;
            } else if (!warnedOnce) {
                warnedOnce = true;
                LOG_WARN("GL shader probes were not installed; will retry on subsequent frames");
            }
        }
    }

    // LoadArea hook - reset area-specific state for new area detection
    static void *Detour_LoadArea(void *thisPtr, void *pAreaNameString, unsigned char a2, unsigned char a3,
                                 unsigned char a4) {
        if (!g_ctx) {
            return H_LoadArea.original()(thisPtr, pAreaNameString, a2, a3, a4);
        }

        LOG_DEBUG("LoadArea called - resetting scale detection for new area");

        auto &ctx = *g_ctx;
        ctx.reset_area_state();
        features::tile_render_state().reset();
        features::clear_disable_request();

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

        auto *result = H_LoadArea.original()(thisPtr, pAreaNameString, a2, a3, a4);
        area::refresh_wed_cache(ctx, thisPtr);
        return result;
    }

    // DrawColorTone hook - simple passthrough with tracking
    static void Detour_DrawColorTone(int mode) {
        H_DrawColorTone.original()(mode);
    }

    // RenderTexture hook - thin dispatch into the tile upscale feature
    static void Detour_RenderTexture(void *thisPtr, int texId, void *unused, int x, int y, unsigned long flags) {
        if (!g_ctx || !g_ctx->manifest) {
            return; // Context destroyed, don't process
        }
        install_shader_probes_once();

        auto &ctx = *g_ctx;

        const bool handled = features::render_tile(ctx, thisPtr, texId, unused, x, y, flags);
        if (!handled) {
            H_RenderTexture.original()(thisPtr, texId, unused, x, y, flags);
        }

        // Feature modules request hook disable (e.g. standard tiles detected);
        // the dispatcher owns the hook objects and performs it. Ordering note:
        // pre-split code disabled before calling the original; calling through
        // original() uses the MinHook trampoline (never re-enters this detour),
        // and rendering is single-threaded, so disabling after is equivalent.
        if (features::should_disable_render_hook()) {
            features::clear_disable_request();
            try {
                H_RenderTexture.disable();
                ctx.isRenderHookActive = false;
                LOG_DEBUG("RenderTexture hook disabled on feature request");
            } catch (const std::exception &e) {
                LOG_WARN("Failed to disable RenderTexture hook: {}", e.what());
            }
        }
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

    void prepare_for_shutdown() noexcept {
        if (g_ctx) {
            g_ctx->isRenderHookActive = false;
        }

        H_DrawColorTone.disable();
        H_RenderTexture.disable();
        H_LoadArea.disable();

        g_ctx = nullptr;
    }

    bool is_active() {
        return g_ctx != nullptr && g_ctx->isRenderHookActive;
    }
}
