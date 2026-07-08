#include "hooks.h"

#include <atomic>
#include <exception>
#include <windows.h>

#include "app_context.h"
#include "area_state.h"
#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/features/tile_render.h"
#include "iee/game/game_types.h"
#include "iee/shader_probe.h"

namespace iee::hooks {
    // region Hook function type definitions
    using Fn_LoadArea = void* (*)(void *, void *, unsigned char, unsigned char, unsigned char);
    using Fn_RenderTexture = void (*)(void *, int, void *, int, int, unsigned long);
    using Fn_DrawColorTone = void (*)(int);

    // Hook management - initialize MinHook
    // Intentionally explicit lifetime: a static smart-pointer destructor would
    // call MinHook from the Windows loader lock if the loader skipped ShutdownBindings.
    static core::HookInit *hkInit = nullptr;
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

        // Publishes the view transform to the uniform feed. Reliable only
        // while the engine is inside its world pass: rViewPort is transient
        // (AdjustViewportForZoom recomputes it as rViewPortNotZoomed * m_fZoom
        // around rendering) — frame-tick-time reads saw restored/stale rects,
        // which broke every map (the reverted v12).
        //
        // The active area is RE-RESOLVED here, not trusted from LoadArea time:
        // on transitions the engine can settle the visible-area pointer after
        // LoadArea returns, which left the cache on the OLD area (mask glued
        // to the screen, or no water at all). When the resolved area differs,
        // re-cache the WED from here — the render thread, where the GL upload
        // belongs anyway. Throttled so a transiently unreadable WED retries
        // once a second instead of every draw.
        void publish_view_state() {
            if (!g_ctx) {
                return;
            }
            // This hook runs inside the world render pass with a current GL
            // context. Flush even when active-area resolution is temporarily
            // unavailable so a queued no-liquid transition clears stale data.
            (void)area::flush_pending_gpu_upload();
            auto *infGame = g_ctx->infGame.load(std::memory_order_relaxed);
            if (!infGame) {
                return;
            }
            const auto *resolved = area::resolve_active_area(infGame, *g_ctx->manifest);
            if (!resolved) {
                return;
            }
            if (resolved != g_ctx->activeArea.load()) {
                static const game::CGameArea *s_lastRefreshTarget = nullptr;
                static std::uint32_t s_lastRefreshTick = 0;
                const auto now = GetTickCount();
                if (resolved != s_lastRefreshTarget || now - s_lastRefreshTick > 1000) {
                    s_lastRefreshTarget = resolved;
                    s_lastRefreshTick = now;
                    LOG_INFO("Active area changed after load; refreshing WED cache from the render thread");
                    area::refresh_wed_cache(*g_ctx, infGame);
                }
            }
            if (!g_ctx->wed.load()) {
                return;
            }
            area::ViewTransform view{};
            if (area::read_view_transform(g_ctx->activeArea.load(), view)) {
                probe::set_area_view(view.scrollX, view.scrollY, view.viewWorldW, view.viewWorldH);
            }
        }
    } // namespace

    // LoadArea hook - reset area-specific state for new area detection
    static void *Detour_LoadArea(void *thisPtr, void *pAreaNameString, unsigned char a2, unsigned char a3,
                                 unsigned char a4) {
        const auto original = H_LoadArea.original();
        if (!g_ctx) {
            return original(thisPtr, pAreaNameString, a2, a3, a4);
        }

        auto &ctx = *g_ctx;
        try {
            LOG_DEBUG("LoadArea called - resetting scale detection for new area");
            ctx.infGame.store(thisPtr, std::memory_order_relaxed);
            ctx.reset_area_state();
            area::reset_gpu_area_state();
            features::tile_render_state().reset();
            features::clear_disable_request();

            // Re-enable RenderTexture hook for new area detection.
            if (!ctx.isRenderHookActive) {
                H_RenderTexture.enable();
                ctx.isRenderHookActive = true;
                LOG_DEBUG("Re-enabled RenderTexture hook for new area detection");
            }
        } catch (const std::exception &e) {
            LOG_ERROR("LoadArea pre-dispatch failed; continuing with the engine path: {}", e.what());
        } catch (...) {
            LOG_ERROR("LoadArea pre-dispatch failed; continuing with the engine path");
        }

        auto *result = original(thisPtr, pAreaNameString, a2, a3, a4);
        try {
            area::refresh_wed_cache(ctx, thisPtr);
            // Seed CPU transform state; the next Seam pass owns the GL upload.
            publish_view_state();
        } catch (const std::exception &e) {
            LOG_ERROR("LoadArea post-dispatch failed; the feature remains disabled for this area: {}", e.what());
            area::reset_gpu_area_state();
        } catch (...) {
            LOG_ERROR("LoadArea post-dispatch failed; the feature remains disabled for this area");
            area::reset_gpu_area_state();
        }
        return result;
    }

    // DrawColorTone hook: the engine calls this throughout rendering (tile,
    // sprite, font tones — decompile 464958/301145/245924). The Seam tone
    // marks the tile pass on ALL map types (the engine's vanilla path and our
    // upscale path both route through it), making it the reliable per-frame
    // publish point with coherent viewport rects. Publish BEFORE the original:
    // the original triggers the fpSEAM bind, which is when the uniform feed
    // reads these values.
    static void Detour_DrawColorTone(int mode) {
        try {
            if (mode == static_cast<int>(game::ShaderTone::Seam)) {
                publish_view_state();
            }
        } catch (...) {
            // Rendering must never depend on IEE diagnostics or uniform state.
        }
        H_DrawColorTone.original()(mode);
    }

    // RenderTexture hook - thin dispatch into the tile upscale feature
    static void Detour_RenderTexture(void *thisPtr, int texId, void *unused, int x, int y, unsigned long flags) {
        const auto original = H_RenderTexture.original();
        if (!g_ctx || !g_ctx->manifest) {
            original(thisPtr, texId, unused, x, y, flags);
            return;
        }

        auto &ctx = *g_ctx;
        bool handled = false;
        try {
            install_shader_probes_once();
            handled = features::render_tile(ctx, thisPtr, texId, unused, x, y, flags);
        } catch (const std::exception &e) {
            LOG_ERROR("RenderTexture enhancement failed; using the engine renderer: {}", e.what());
        } catch (...) {
            LOG_ERROR("RenderTexture enhancement failed; using the engine renderer");
        }
        if (!handled) {
            original(thisPtr, texId, unused, x, y, flags);
        }

        // Feature modules request hook disable (e.g. standard tiles detected);
        // the dispatcher owns the hook objects and performs it. Ordering note:
        // pre-split code disabled before calling the original; calling through
        // original() uses the MinHook trampoline (never re-enters this detour),
        // and rendering is single-threaded, so disabling after is equivalent.
        if (features::should_disable_render_hook()) {
            features::clear_disable_request();
            if (H_RenderTexture.disable()) {
                ctx.isRenderHookActive = false;
                LOG_DEBUG("RenderTexture hook disabled on feature request");
            } else {
                LOG_WARN("Failed to disable RenderTexture hook");
            }
        }
    }

    bool install_all(AppContext &ctx) {
        g_ctx = &ctx;

        try {
            if (!hkInit) hkInit = new core::HookInit();
            H_LoadArea.create(reinterpret_cast<void *>(ctx.addrs.LoadArea), (void *)&Detour_LoadArea);
            LOG_INFO("LoadArea hook created");

            H_RenderTexture.create(reinterpret_cast<void *>(ctx.addrs.RenderTexture), (void *)&Detour_RenderTexture);
            LOG_INFO("RenderTexture hook created");

            if (ctx.draw.DrawColorTone) {
                try {
                    H_DrawColorTone.create(reinterpret_cast<void *>(ctx.draw.DrawColorTone),
                                           (void *)&Detour_DrawColorTone);
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
            (void)H_DrawColorTone.remove();
            (void)H_RenderTexture.remove();
            (void)H_LoadArea.remove();
            g_ctx = nullptr;
            delete hkInit;
            hkInit = nullptr;
            return false;
        } catch (...) {
            LOG_ERROR("Unknown exception during hook installation");
            (void)H_DrawColorTone.remove();
            (void)H_RenderTexture.remove();
            (void)H_LoadArea.remove();
            g_ctx = nullptr;
            delete hkInit;
            hkInit = nullptr;
            return false;
        }
    }

    void uninstall_all() noexcept {
        try {
            LOG_INFO("Uninstalling all hooks...");
        } catch (...) {
        }

        (void)H_DrawColorTone.remove();
        (void)H_RenderTexture.remove();
        (void)H_LoadArea.remove();

        g_ctx = nullptr;
        delete hkInit;
        hkInit = nullptr;

        try {
            LOG_INFO("Hook cleanup complete");
        } catch (...) {
        }
    }

    void prepare_for_shutdown() noexcept {
        if (g_ctx) {
            g_ctx->isRenderHookActive = false;
        }

        uninstall_all();
    }

    bool is_active() { return g_ctx != nullptr && g_ctx->isRenderHookActive; }
} // namespace iee::hooks
