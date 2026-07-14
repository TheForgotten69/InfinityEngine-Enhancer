#include "hooks.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>

#include "app_context.h"
#include "area_state.h"
#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/features/tile_render.h"
#include "iee/frame_hook.h"
#include "iee/game/game_types.h"
#include "iee/game/renderer.h"
#include "iee/shader_probe.h"

namespace iee::hooks {
using LoadAreaFn = void* (*)(void*, void*, unsigned char, unsigned char, unsigned char);
using RenderTextureFn = void (*)(void*, int, void*, int, int, unsigned long);
using DrawColorToneFn = void (*)(int);

// Hook management - initialize MinHook
// Intentionally explicit lifetime: a static smart-pointer destructor would
// call MinHook from the Windows loader lock if the loader skipped ShutdownBindings.
static core::HookInit* g_hookInit = nullptr;
static core::Hook<LoadAreaFn> g_loadAreaHook;
static core::Hook<RenderTextureFn> g_renderTextureHook;
static core::Hook<DrawColorToneFn> g_drawColorToneHook;

static AppContext* g_ctx = nullptr;

namespace {
void record_render_performance(bool enabled, bool handled, long long elapsedTicks) noexcept {
  if (!enabled || elapsedTicks < 0) return;

  try {
    static const long long frequency = [] {
      LARGE_INTEGER value{};
      return QueryPerformanceFrequency(&value) ? value.QuadPart : 0LL;
    }();
    if (frequency <= 0) return;

    struct Window {
      long long startedAt{};
      long long totalTicks{};
      long long maximumTicks{};
      unsigned long long calls{};
      unsigned long long handledCalls{};
    };
    static Window window;

    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) return;
    if (window.startedAt == 0) window.startedAt = now.QuadPart;
    window.totalTicks += elapsedTicks;
    window.maximumTicks = (std::max)(window.maximumTicks, elapsedTicks);
    ++window.calls;
    if (handled) ++window.handledCalls;

    constexpr long long kReportSeconds = 5;
    if (now.QuadPart - window.startedAt < frequency * kReportSeconds) return;

    const double ticksToMicroseconds = 1'000'000.0 / static_cast<double>(frequency);
    const double averageMicroseconds = static_cast<double>(window.totalTicks) *
                                       ticksToMicroseconds / static_cast<double>(window.calls);
    const double maximumMicroseconds =
        static_cast<double>(window.maximumTicks) * ticksToMicroseconds;
    LOG_INFO(
        "RenderTexture enhancement perf: calls={}, handled={}, delegated={}, avg={:.2f}us, "
        "max={:.2f}us over {}s",
        window.calls, window.handledCalls, window.calls - window.handledCalls, averageMicroseconds,
        maximumMicroseconds, kReportSeconds);
    window = {.startedAt = now.QuadPart};
  } catch (...) {
    // Performance diagnostics must not affect rendering.
  }
}

void install_shader_probes_once() {
  // Latch only on success: a transient first-frame failure (partial GL
  // table) must not permanently suppress the probes. Runs on the render
  // thread only, so plain statics are safe.
  static bool installed = false;
  static bool warnedOnce = false;
  static std::uint32_t lastAttemptTick = 0;
  if (installed) {
    return;
  }
  const auto now = GetTickCount();
  if (lastAttemptTick != 0 && now - lastAttemptTick < 1000) {
    return;
  }
  lastAttemptTick = now;
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

// Publishes the view transform while the engine's transient world-pass
// viewport is coherent.
// The active area is RE-RESOLVED here, not trusted from LoadArea time:
// on transitions the engine can settle the visible-area pointer after
// LoadArea returns, which left the cache on the OLD area (mask glued
// to the screen, or no water at all). When the resolved area differs,
// re-cache the WED from here — the render thread, where the GL upload
// belongs anyway. Throttled so a transiently unreadable WED retries
// once a second instead of every draw.
void publish_view_state(bool force = false, bool flushGpuUpload = true) {
  if (!g_ctx) {
    return;
  }
  static unsigned long long lastPublishedFrame = 0;
  const auto frameNumber = frame::frame_count();
  if (!force && frameNumber != 0 && frameNumber == lastPublishedFrame) {
    return;
  }
  // A LoadArea CPU-only publication must not consume the render callback's
  // once-per-frame slot; the next Seam callback still owns the queued upload.
  if (flushGpuUpload && frameNumber != 0) {
    lastPublishedFrame = frameNumber;
  }
  if (flushGpuUpload) {
    // DrawColorTone runs inside the world render pass with a current GL
    // context. Flush even when active-area resolution is temporarily
    // unavailable so a queued no-liquid transition clears stale data.
    (void)area::flush_pending_gpu_upload();
  }
  auto* infGame = g_ctx->infGame.load(std::memory_order_relaxed);
  if (!infGame) {
    return;
  }
  const auto* resolved = area::resolve_active_area(infGame, *g_ctx->manifest);
  if (!resolved) {
    return;
  }
  if (resolved != g_ctx->activeArea.load()) {
    static const game::CGameArea* s_lastRefreshTarget = nullptr;
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
}  // namespace

// LoadArea hook - reset area-specific state for new area detection
static void* detour_load_area(void* thisPtr, void* pAreaNameString, unsigned char a2,
                              unsigned char a3, unsigned char a4) {
  const auto original = g_loadAreaHook.original();
  if (!g_ctx) {
    return original(thisPtr, pAreaNameString, a2, a3, a4);
  }

  auto& ctx = *g_ctx;
  try {
    LOG_DEBUG("LoadArea called - resetting scale detection for new area");
    ctx.infGame.store(thisPtr, std::memory_order_relaxed);
    ctx.reset_area_state();
    area::reset_gpu_area_state();
    features::request_tile_render_state_reset();
    game::request_texture_configuration_cache_reset();
    features::clear_disable_request();

    // Re-enable RenderTexture hook for new area detection.
    if (!ctx.isRenderHookActive.load(std::memory_order_relaxed)) {
      g_renderTextureHook.enable();
      ctx.isRenderHookActive.store(true, std::memory_order_relaxed);
      LOG_DEBUG("Re-enabled RenderTexture hook for new area detection");
    }
  } catch (const std::exception& e) {
    LOG_ERROR("LoadArea pre-dispatch failed; continuing with the engine path: {}", e.what());
  } catch (...) {
    LOG_ERROR("LoadArea pre-dispatch failed; continuing with the engine path");
  }

  auto* result = original(thisPtr, pAreaNameString, a2, a3, a4);
  try {
    area::refresh_wed_cache(ctx, thisPtr);
    // Seed CPU transform state; the next Seam pass owns the GL upload.
    publish_view_state(true, false);
  } catch (const std::exception& e) {
    LOG_ERROR("LoadArea post-dispatch failed; the feature remains disabled for this area: {}",
              e.what());
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
static void detour_draw_color_tone(int mode) {
  try {
    if (mode == static_cast<int>(game::ShaderTone::Seam)) {
      publish_view_state();
    }
  } catch (...) {
    // Rendering must never depend on IEE diagnostics or uniform state.
  }
  g_drawColorToneHook.original()(mode);
}

// RenderTexture hook - thin dispatch into the tile upscale feature
static void detour_render_texture(void* thisPtr, int texId, void* unused, int x, int y,
                                  unsigned long flags) {
  const auto original = g_renderTextureHook.original();
  if (!g_ctx || !g_ctx->manifest) {
    original(thisPtr, texId, unused, x, y, flags);
    return;
  }

  auto& ctx = *g_ctx;
  bool handled = false;
  LARGE_INTEGER performanceStart{};
  const bool measurePerformance =
      ctx.cfg.enablePerformanceLogging && QueryPerformanceCounter(&performanceStart);
  try {
    install_shader_probes_once();
    handled = features::render_tile(ctx, thisPtr, texId, unused, x, y, flags);
  } catch (const std::exception& e) {
    LOG_ERROR("RenderTexture enhancement failed; using the engine renderer: {}", e.what());
  } catch (...) {
    LOG_ERROR("RenderTexture enhancement failed; using the engine renderer");
  }
  if (measurePerformance) {
    LARGE_INTEGER performanceEnd{};
    if (QueryPerformanceCounter(&performanceEnd)) {
      record_render_performance(true, handled, performanceEnd.QuadPart - performanceStart.QuadPart);
    }
  }
  if (!handled) {
    original(thisPtr, texId, unused, x, y, flags);
  }

  // The dispatcher owns hook lifetime. Disable only after the current draw
  // has completed through either the enhanced path or the trampoline.
  if (features::should_disable_render_hook()) {
    features::clear_disable_request();
    if (g_renderTextureHook.disable()) {
      ctx.isRenderHookActive.store(false, std::memory_order_relaxed);
      LOG_DEBUG("RenderTexture hook disabled on feature request");
    } else {
      LOG_WARN("Failed to disable RenderTexture hook");
    }
  }
}

bool install_all(AppContext& ctx) {
  g_ctx = &ctx;

  try {
    if (!g_hookInit) g_hookInit = new core::HookInit();
    g_loadAreaHook.create(reinterpret_cast<void*>(ctx.addrs.LoadArea),
                          reinterpret_cast<void*>(&detour_load_area));
    LOG_INFO("LoadArea hook created");

    g_renderTextureHook.create(reinterpret_cast<void*>(ctx.addrs.RenderTexture),
                               reinterpret_cast<void*>(&detour_render_texture));
    LOG_INFO("RenderTexture hook created");

    if (ctx.draw.DrawColorTone) {
      try {
        g_drawColorToneHook.create(reinterpret_cast<void*>(ctx.draw.DrawColorTone),
                                   reinterpret_cast<void*>(&detour_draw_color_tone));
        g_drawColorToneHook.enable();
        LOG_INFO("DrawColorTone hook installed");
      } catch (const std::exception& e) {
        ctx.cfg.enableWaterEffect = false;
        ctx.cfg.enableDebugHotkeys = false;
        LOG_ERROR(
            "Water effect disabled: DrawColorTone hook installation failed ({}); a coherent "
            "world-view transform cannot be published safely. Tile upscaling remains enabled.",
            e.what());
      } catch (...) {
        ctx.cfg.enableWaterEffect = false;
        ctx.cfg.enableDebugHotkeys = false;
        LOG_ERROR(
            "Water effect disabled: DrawColorTone hook installation failed with an unknown "
            "error; a coherent world-view transform cannot be published safely. Tile "
            "upscaling remains enabled.");
      }
    } else {
      ctx.cfg.enableWaterEffect = false;
      ctx.cfg.enableDebugHotkeys = false;
      LOG_ERROR(
          "Water effect disabled: DrawColorTone was not resolved; a coherent world-view "
          "transform cannot be published safely. Tile upscaling remains enabled.");
    }

    g_loadAreaHook.enable();
    LOG_INFO("LoadArea hook enabled");

    g_renderTextureHook.enable();
    LOG_INFO("RenderTexture hook enabled");

    ctx.isRenderHookActive.store(true, std::memory_order_relaxed);

    LOG_INFO("All hooks installed successfully");
    LOG_INFO("LoadArea: 0x{:X}", ctx.addrs.LoadArea);
    LOG_INFO("RenderTexture: 0x{:X}", ctx.addrs.RenderTexture);
    LOG_INFO("RenderTexture hook enabled - will detect upscaled textures automatically");

    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("Exception during hook installation: {}", e.what());
    (void)g_drawColorToneHook.remove();
    (void)g_renderTextureHook.remove();
    (void)g_loadAreaHook.remove();
    g_ctx = nullptr;
    delete g_hookInit;
    g_hookInit = nullptr;
    return false;
  } catch (...) {
    LOG_ERROR("Unknown exception during hook installation");
    (void)g_drawColorToneHook.remove();
    (void)g_renderTextureHook.remove();
    (void)g_loadAreaHook.remove();
    g_ctx = nullptr;
    delete g_hookInit;
    g_hookInit = nullptr;
    return false;
  }
}

void uninstall_all() noexcept {
  try {
    LOG_INFO("Uninstalling all hooks...");
  } catch (...) {
  }

  (void)g_drawColorToneHook.remove();
  (void)g_renderTextureHook.remove();
  (void)g_loadAreaHook.remove();

  g_ctx = nullptr;
  delete g_hookInit;
  g_hookInit = nullptr;

  try {
    LOG_INFO("Hook cleanup complete");
  } catch (...) {
  }
}

void prepare_for_shutdown() noexcept {
  if (g_ctx) {
    g_ctx->isRenderHookActive.store(false, std::memory_order_relaxed);
  }
  // Quiesce engine entry points before dependent frame/GL hooks and shared
  // state are torn down. MinHook itself stays initialized until
  // uninstall_all(), after every MinHook-backed subsystem has removed its
  // hooks.
  (void)g_drawColorToneHook.disable();
  (void)g_renderTextureHook.disable();
  (void)g_loadAreaHook.disable();
  g_ctx = nullptr;
}

bool is_active() {
  // The RenderTexture hook is intentionally disabled on standard-resolution
  // areas while the DLL, area hooks, and shader features remain active.
  return g_ctx != nullptr;
}
}  // namespace iee::hooks
