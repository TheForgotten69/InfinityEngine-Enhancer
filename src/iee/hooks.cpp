#include "hooks.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include "app_context.h"
#include "area_state.h"
#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"
#include "iee/core/performance_samples.h"
#include "iee/features/tile_render.h"
#include "iee/features/world_post.h"
#include "iee/frame_hook.h"
#include "iee/game/game_types.h"
#include "iee/game/animation_interp.h"
#include "iee/game/draw_queue.h"
#include "iee/game/renderer.h"
#include "iee/game/runtime_types_x64.h"
#include "iee/game/sprite_motion.h"
#include "iee/shader_probe.h"

namespace iee::hooks {
using LoadAreaFn = void* (*)(void*, void*, unsigned char, unsigned char, unsigned char);
using RenderTextureFn = void (*)(void*, int, void*, int, int, unsigned long);
using DrawColorToneFn = void (*)(int);
using StaticRenderFn = void (*)(void*, void*, void*);

// Hook management - initialize MinHook
// Intentionally explicit lifetime: a static smart-pointer destructor would
// call MinHook from the Windows loader lock if the loader skipped ShutdownBindings.
static core::HookInit* g_hookInit = nullptr;
static core::Hook<LoadAreaFn> g_loadAreaHook;
static core::Hook<RenderTextureFn> g_renderTextureHook;
static core::Hook<DrawColorToneFn> g_drawColorToneHook;
static core::Hook<StaticRenderFn> g_staticRenderHook;
// CGameSprite::Render(area, vidMode) and the (vidMode) overlay renders share
// one pass-through signature; the unused third argument is harmless on x64.
using SpriteRenderFn = void (*)(void*, void*, void*);
static core::Hook<SpriteRenderFn> g_spriteRenderHook;
static core::Hook<SpriteRenderFn> g_spriteMarkersHook;
static core::Hook<SpriteRenderFn> g_spriteHealthBarHook;
static std::array<core::Hook<SpriteRenderFn>, game::kMaxSmoothedObjectRenders> g_objectRenderHooks;
// CVidCell::GetFrame(), GetCurrentCenterPoint(CPoint&), GetCurrentFrameSize(CSize&):
// one pass-through signature, the unused argument is harmless on x64.
using VidCellAccessorFn = int (*)(void*, void*);
static core::Hook<VidCellAccessorFn> g_vidCellGetFrameHook;
static core::Hook<VidCellAccessorFn> g_vidCellCenterPointHook;
static core::Hook<VidCellAccessorFn> g_vidCellFrameSizeHook;
// CInfinity::RenderFog(CVidMode*, CVisibilityMap*) and the GL backend's
// argument-less queue flush, used together by the world post passes.
using RenderFogFn = void (*)(void*, void*, void*);
using DrawFlushFn = void (*)();
static core::Hook<RenderFogFn> g_renderFogHook;
static DrawFlushFn g_drawFlush = nullptr;
// DrawFlush_GL is also detoured, to replay the queue's additive draws into
// the bloom's emissive target. g_drawFlush stays the engine's entry point, so
// our own flushes go through the detour too.
static core::Hook<DrawFlushFn> g_drawFlushHook;
static std::int32_t* g_drawQueueCount = nullptr;
static game::DrawCommand* g_drawQueueCommands = nullptr;
// TexSubImage_GL(x, y, width, rows, pixels, secondTexture): the engine's
// texture upload, hooked for the sprite atlas.
using TextureUploadFn = void (*)(int, int, int, int, void*, bool);
static core::Hook<TextureUploadFn> g_textureUploadHook;
// Atlas textures replaced by their upscaled copies for the current flush.
struct SwappedAtlas {
  std::uint32_t* name{};
  std::uint32_t original{};
};
static std::array<SwappedAtlas, 2> g_swappedAtlases{};

// CParticle::AsynchronousUpdate() and CParticle::Render(CPoint&, CRect&, type, n).
using ParticleUpdateFn = unsigned char (*)(void*);
using ParticleRenderFn = void (*)(void*, void*, void*, unsigned short, unsigned short);
static core::Hook<ParticleUpdateFn> g_particleUpdateHook;
static core::Hook<ParticleRenderFn> g_particleRenderHook;

static AppContext* g_ctx = nullptr;
// Raised by LoadArea; the sprite-smoothing tracker clears itself on its own thread.
static std::atomic<bool> g_spriteMotionReset{false};

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
      unsigned long long activeFrame{};
      long long activeFrameTicks{};
      core::PerformanceSamples<2048> frameCpuMs;

      void finish_frame(double ticksToMilliseconds) noexcept {
        if (activeFrame != 0) {
          frameCpuMs.add(static_cast<double>(activeFrameTicks) * ticksToMilliseconds);
        }
        activeFrameTicks = 0;
      }

      void reset(long long nextStart) noexcept {
        startedAt = nextStart;
        totalTicks = 0;
        maximumTicks = 0;
        calls = 0;
        handledCalls = 0;
        frameCpuMs.reset();
      }
    };
    static Window window;

    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) return;
    if (window.startedAt == 0) window.startedAt = now.QuadPart;
    window.totalTicks += elapsedTicks;
    window.maximumTicks = (std::max)(window.maximumTicks, elapsedTicks);
    ++window.calls;
    if (handled) ++window.handledCalls;

    const auto frameNumber = frame::frame_count();
    const double ticksToMilliseconds = 1000.0 / static_cast<double>(frequency);
    if (frameNumber != 0 && frameNumber != window.activeFrame) {
      window.finish_frame(ticksToMilliseconds);
      window.activeFrame = frameNumber;
    }
    if (frameNumber != 0) window.activeFrameTicks += elapsedTicks;

    constexpr long long kReportSeconds = 5;
    if (now.QuadPart - window.startedAt < frequency * kReportSeconds) return;

    const double ticksToMicroseconds = 1'000'000.0 / static_cast<double>(frequency);
    const double averageMicroseconds = static_cast<double>(window.totalTicks) *
                                       ticksToMicroseconds / static_cast<double>(window.calls);
    const double maximumMicroseconds =
        static_cast<double>(window.maximumTicks) * ticksToMicroseconds;
    const auto frameSummary = window.frameCpuMs.summarize();
    const auto readability = core::take_readability_stats();
    const auto textureStats = game::take_texture_configuration_stats();
    LOG_INFO(
        "RenderTexture enhancement perf: calls={}, handled={}, delegated={}, avg={:.2f}us, "
        "max={:.2f}us; per-frame CPU samples={}, avg={:.2f}ms, p95={:.2f}ms, max={:.2f}ms "
        "over {}s; safe-read cache hits={}, VirtualQuery calls={}; texture config calls={}, "
        "cacheHits={}, configured={}, latchedFailures={}, evictions={}",
        window.calls, window.handledCalls, window.calls - window.handledCalls, averageMicroseconds,
        maximumMicroseconds, frameSummary.count, frameSummary.average, frameSummary.percentile95,
        frameSummary.maximum, kReportSeconds, readability.cacheHits, readability.virtualQueries,
        textureStats.calls, textureStats.cacheHits, textureStats.configured,
        textureStats.latchedFailures, textureStats.evictions);
    window.reset(now.QuadPart);
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
  static bool publishedAtFrameZero = false;
  static std::uint32_t lastFallbackPublishTick = 0;
  const auto frameNumber = frame::frame_count();
  if (!force && frameNumber != 0 && frameNumber == lastPublishedFrame) {
    return;
  }
  if (!force && flushGpuUpload && frameNumber == 0) {
    if (frame::boundary_available()) {
      if (publishedAtFrameZero) return;
      publishedAtFrameZero = true;
    } else {
      // Unsupported fallback: coalesce a burst of per-tile Seam calls while
      // still allowing camera state to advance when no swap hook exists.
      const auto now = GetTickCount();
      if (lastFallbackPublishTick != 0 && now - lastFallbackPublishTick < 8) return;
      lastFallbackPublishTick = now;
      // Without a swap hook nothing else advances the safe-read epoch, and a
      // stale readability cache must never outlive engine resource churn.
      core::advance_readability_cache_epoch();
    }
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
  area::republish_area_animations_if_dirty(*g_ctx);
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
    core::advance_readability_cache_epoch();
    LOG_INFO("LoadArea called on thread {} - resetting scale detection for new area",
              GetCurrentThreadId());
    ctx.infGame.store(thisPtr, std::memory_order_relaxed);
    // Invalidate any older refresh before clearing its published CPU state.
    area::reset_gpu_area_state();
    ctx.reset_area_state();
    features::request_tile_render_state_reset();
    g_spriteMotionReset.store(true, std::memory_order_release);
    features::world_post_on_area_load();
    game::request_texture_configuration_cache_reset();
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

// Sprite movement smoothing. The engine moves creatures on its logic tick
// (30 Hz) while EEex's uncapped renderer draws many frames per tick, so every
// render that reads the sprite position is wrapped: the smoothed position is
// written into CGameObject::m_pos for the duration of the engine call and the
// logic position is restored afterwards. All of it runs on the engine's main
// thread; LoadArea only raises the reset flag.
static game::SpriteMotionTracker g_spriteMotion;
// Projectiles and effects cover far more ground per tick than creatures.
static game::SpriteMotionTracker g_objectMotion{192};
// Draw-time animation interpolation state (see detour_sprite_render).
static game::AnimationInterpolator g_animationInterp;
static bool g_animationInterpActive = false;
static int g_spritePositionSwapDepth = 0;

// One timestamp per presented frame, so a sprite, its selection circle and
// its health bar are all placed at the same smoothed position.
static double sprite_motion_frame_seconds() noexcept {
  static unsigned long long lastFrame = ~0ull;
  static double seconds = 0.0;
  const auto frameNumber = frame::frame_count();
  if (frameNumber != lastFrame || frameNumber == 0) {
    lastFrame = frameNumber;
    static const double frequency = [] {
      LARGE_INTEGER value{};
      QueryPerformanceFrequency(&value);
      return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    seconds = static_cast<double>(counter.QuadPart) / frequency;
  }
  return seconds;
}

static void call_with_smoothed_position(core::Hook<SpriteRenderFn>& hook,
                                        game::SpriteMotionTracker& tracker, void* sprite,
                                        void* a, void* b) {
  const auto original = hook.original();
  // Nested sprite renders already see the smoothed position.
  if (!sprite || !g_ctx || g_spritePositionSwapDepth > 0 || !g_ctx->cfg.smoothSpriteMovement) {
    original(sprite, a, b);
    return;
  }
  auto* position = reinterpret_cast<game::CPoint*>(static_cast<std::byte*>(sprite) +
                                                   offsetof(game::CGameObject, m_pos));
  game::CPoint logic{};
  std::int32_t shownX = 0;
  std::int32_t shownY = 0;
  bool swapped = false;
  try {
    static bool threadLogged = false;
    if (!threadLogged) {
      threadLogged = true;
      LOG_INFO("Sprite smoothing: first sprite render on thread {}", GetCurrentThreadId());
    }
    if (g_spriteMotionReset.exchange(false, std::memory_order_acq_rel)) {
      g_spriteMotion.clear();
      g_objectMotion.clear();
      g_animationInterp.reset_timing();
    }
    if (core::safe_read(position, logic)) {
      const auto shown =
          tracker.sample(sprite, {logic.x, logic.y}, sprite_motion_frame_seconds());
      if (shown.x != logic.x || shown.y != logic.y) {
        shownX = shown.x;
        shownY = shown.y;
        position->x = shownX;
        position->y = shownY;
        swapped = true;
      }
    }
  } catch (...) {
    // Smoothing is cosmetic; any doubt draws at the logic position.
  }
  ++g_spritePositionSwapDepth;
  original(sprite, a, b);
  --g_spritePositionSwapDepth;
  // Restore only our own write: if the engine moved the sprite meanwhile, its
  // value is the newer logic position and must win.
  if (swapped && position->x == shownX && position->y == shownY) *position = logic;
}

// Animation interpolation. While a creature is being drawn, every CVidCell the
// engine resolves a frame for is pointed at its expanded BAM and the in-between
// frame for this instant (see game::AnimationInterpolator); the logic's
// resource and frame are restored when the draw returns. Main thread only.

static void detour_sprite_render(void* sprite, void* a, void* b) {
  if (!g_animationInterpActive) {
    call_with_smoothed_position(g_spriteRenderHook, g_spriteMotion, sprite, a, b);
    return;
  }
  g_animationInterp.begin_scope(sprite);
  call_with_smoothed_position(g_spriteRenderHook, g_spriteMotion, sprite, a, b);
  g_animationInterp.end_scope();
}
static int detour_vid_cell_get_frame(void* cell, void* unused) {
  g_animationInterp.touch(static_cast<game::CVidCell*>(cell), sprite_motion_frame_seconds());
  return g_vidCellGetFrameHook.original()(cell, unused);
}
static int detour_vid_cell_center_point(void* cell, void* out) {
  g_animationInterp.touch(static_cast<game::CVidCell*>(cell), sprite_motion_frame_seconds());
  return g_vidCellCenterPointHook.original()(cell, out);
}
static int detour_vid_cell_frame_size(void* cell, void* out) {
  g_animationInterp.touch(static_cast<game::CVidCell*>(cell), sprite_motion_frame_seconds());
  return g_vidCellFrameSizeHook.original()(cell, out);
}
// Particle smoothing (rain, snow, sparkles). The engine adds each particle's
// velocity to its position once per logic tick, so between ticks its previous
// position is known exactly. Drawing it part of the way from there removes
// the stepping. The update may run on another thread: the tick clock is
// atomic, and the position is only put back if the engine has not moved the
// particle in the meantime.
static game::TickClock g_particleTick;

static double monotonic_seconds() noexcept {
  static const double frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<double>(value.QuadPart);
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) / frequency;
}

static unsigned char detour_particle_update(void* particle) {
  g_particleTick.on_update(monotonic_seconds());
  return g_particleUpdateHook.original()(particle);
}

static void detour_particle_render(void* self, void* origin, void* clip, unsigned short type,
                                   unsigned short count) {
  const auto original = g_particleRenderHook.original();
  auto* particle = static_cast<game::CParticle*>(self);
  const double phase = g_particleTick.phase(sprite_motion_frame_seconds());
  if (!particle || phase >= 1.0) {
    original(self, origin, clip, type, count);
    return;
  }
  const game::ParticlePoint logic{particle->m_posX, particle->m_posY, particle->m_posZ};
  const auto shown = game::particle_draw_position(
      {logic,
       {particle->m_velX, particle->m_velY, particle->m_velZ},
       particle->m_nGravity,
       (particle->m_wType & 1) != 0},
      phase);
  particle->m_posX = shown.x;
  particle->m_posY = shown.y;
  particle->m_posZ = shown.z;
  original(self, origin, clip, type, count);
  if (particle->m_posX == shown.x && particle->m_posY == shown.y && particle->m_posZ == shown.z) {
    particle->m_posX = logic.x;
    particle->m_posY = logic.y;
    particle->m_posZ = logic.z;
  }
}

static void detour_sprite_markers(void* sprite, void* a, void* b) {
  call_with_smoothed_position(g_spriteMarkersHook, g_spriteMotion, sprite, a, b);
}
static void detour_sprite_health_bar(void* sprite, void* a, void* b) {
  call_with_smoothed_position(g_spriteHealthBarHook, g_spriteMotion, sprite, a, b);
}

// One detour per manifest slot, so each knows which original to call.
template <std::size_t Slot>
static void detour_object_render(void* object, void* a, void* b) {
  call_with_smoothed_position(g_objectRenderHooks[Slot], g_objectMotion, object, a, b);
}
template <std::size_t... Slots>
static constexpr std::array<SpriteRenderFn, sizeof...(Slots)> object_render_detours(
    std::index_sequence<Slots...>) {
  return {&detour_object_render<Slots>...};
}
static constexpr auto kObjectRenderDetours =
    object_render_detours(std::make_index_sequence<game::kMaxSmoothedObjectRenders>{});

// Bloom source. The engine draws its light-emitting art (fires, spell
// effects, glows) additively, and every queued command records its blend
// mode. During the world pass each flush is followed by a second one that
// holds only those commands, with our black emissive target bound. The vertex
// data and textures of the first flush are still in place: the engine only
// overwrites them when new draws are queued. Render thread only.
static void flush_with_emissive_replay() {
  const auto original = g_drawFlushHook.original();
  if (!g_ctx || !g_drawQueueCount || !features::world_post_wants_emissive()) {
    original();
    return;
  }
  static std::array<game::DrawCommand, 1024> additive;
  const auto collected = game::collect_additive(g_ctx->manifest->drawQueue, g_drawQueueCommands,
                                                *g_drawQueueCount, additive);
  original();
  if (collected == 0 || !features::world_post_begin_emissive()) return;
  std::memcpy(g_drawQueueCommands, additive.data(), collected * sizeof(game::DrawCommand));
  *g_drawQueueCount = static_cast<std::int32_t>(collected);
  original();
  features::world_post_end_emissive(static_cast<int>(collected));
}

// Field counters while the sprite path is being verified in game.
static std::atomic<unsigned> g_flushCalls{0};
static std::atomic<unsigned> g_uploadCalls{0};

static void log_flush_diagnostics() {
  // The first flushes that have draws queued: how many rows each sprite atlas
  // has pending (what the engine is about to upload), and the hook counters.
  static int logged = 0;
  if (logged >= 6 || !g_drawQueueCount || *g_drawQueueCount <= 0) return;
  ++logged;
  const auto count = reinterpret_cast<std::uintptr_t>(g_drawQueueCount);
  const auto word = [&](std::ptrdiff_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(count + offset), sizeof(value));
    return value;
  };
  LOG_INFO(
      "Flush #{} ({} flush calls, {} upload calls so far): {} commands | atlas0 pending "
      "y={} h={} texture={} | atlas1 pending y={} h={} texture={} | wants upscale={}",
      logged, g_flushCalls.load(), g_uploadCalls.load(), *g_drawQueueCount, word(0x94), word(0x98),
      word(0xA8), word(0xC4), word(0xC8), word(0xD8), features::world_post_wants_atlas_upscale());
}

static void detour_draw_flush() {
  ++g_flushCalls;
  try {
    log_flush_diagnostics();
  } catch (...) {
  }
  flush_with_emissive_replay();
  // The engine uploads into, and binds, whatever name its texture table
  // holds: give it its own atlas textures back before the next upload.
  for (auto& swapped : g_swappedAtlases) {
    if (swapped.name) *swapped.name = swapped.original;
    swapped = {};
  }
}

// Sprite smoothing. A flush starts by uploading the CPU-composited sprite
// atlas, then draws from it. Right after that upload the atlas is upscaled
// (FSR1) into our own texture, whose name replaces the atlas's in the engine's
// texture table until the flush is over. The upload has just marked the
// texture as needing a rebind, so the first draw picks ours up.
static void detour_texture_upload(int x, int y, int width, int rows, void* pixels, bool second) {
  ++g_uploadCalls;
  g_textureUploadHook.original()(x, y, width, rows, pixels, second);
  if (!g_ctx || !g_drawQueueCount) return;
  const auto& layout = g_ctx->manifest->spriteAtlas;
  const auto count = reinterpret_cast<std::uintptr_t>(g_drawQueueCount);
  // Field log: the first uploads seen while sprite upscaling wants to run,
  // with everything the atlas lookup compares, so a mismatch is visible.
  static int reached = 0;
  if (reached < 4) {
    ++reached;
    try {
      LOG_INFO("Texture upload hook reached (#{}, width={}, rows={}, wants upscale={})", reached,
               width, rows, features::world_post_wants_atlas_upscale());
    } catch (...) {
    }
  }
  static int logged = 0;
  if (logged < 12 && features::world_post_wants_atlas_upscale()) {
    ++logged;
    try {
      const auto word = [&](std::ptrdiff_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(count + offset), sizeof(value));
        return value;
      };
      const auto pointer = [&](std::ptrdiff_t offset) {
        std::uintptr_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(count + offset), sizeof(value));
        return value;
      };
      LOG_INFO(
          "Texture upload #{}: x={} y={} width={} rows={} second={} pixels=0x{:X} | selected "
          "texture={} | atlas0: {}x{} buffer=0x{:X} texture={} | atlas1: {}x{} buffer=0x{:X} "
          "texture={} | slot={}",
          logged, x, y, width, rows, second, reinterpret_cast<std::uintptr_t>(pixels),
          (word(-0x64) >> 21) & 0x1FF, word(0x80), word(0x84), pointer(0xA0), word(0xA8),
          word(0xB0), word(0xB4), pointer(0xD0), word(0xD8),
          game::atlas_slot_for_upload(layout, count, pixels));
    } catch (...) {
    }
  }
  if (second || x != 0 || y != 0 || !features::world_post_wants_atlas_upscale()) return;
  const int slot = game::atlas_slot_for_upload(layout, count, pixels);
  if (slot < 0 || static_cast<std::size_t>(slot) >= g_swappedAtlases.size()) return;
  auto& swapped = g_swappedAtlases[static_cast<std::size_t>(slot)];
  const auto atlas = game::atlas_info(layout, count, slot);
  if (!atlas.textureName || swapped.name || atlas.width != width) return;
  const unsigned upscaled = features::world_post_upscale_atlas(slot, *atlas.textureName,
                                                               atlas.width, atlas.height, rows);
  if (!upscaled) return;
  swapped = {atlas.textureName, *atlas.textureName};
  *atlas.textureName = upscaled;
}

// World post passes (soft fog of war, bloom). The engine queues every draw
// and submits the queue in DrawFlush_GL at the end of the frame, so the world
// only exists in the framebuffer once we flush, and the fog only lands in our
// target if we flush again while it is bound. With both effects off this
// detour is a plain pass-through: no flush, the frame is the engine's own.
static void detour_render_fog(void* infinity, void* vidMode, void* visibility) {
  const auto original = g_renderFogHook.original();
  if (g_ctx && g_ctx->cfg.enableDebugHotkeys) features::world_post_poll_hotkeys();
  if (!g_ctx || !g_drawFlush || !features::world_post_active()) {
    original(infinity, vidMode, visibility);
    return;
  }
  features::WorldView worldView{};
  if (const auto* activeArea = g_ctx->activeArea.load()) {
    area::ViewTransform view{};
    if (area::read_view_transform(activeArea, view)) {
      worldView = {view.scrollX, view.scrollY, view.viewWorldW, view.viewWorldH};
    }
  }
  g_drawFlush();
  const bool capturing = features::world_post_before_fog(worldView);
  original(infinity, vidMode, visibility);
  if (capturing) {
    g_drawFlush();
    features::world_post_after_fog();
  }
}

// CGameStatic::Render hook: while the fpSEAM point effects are active, the
// authored fire/smoke BAM draws are replaced by our textured effects, so the
// engine's own little flame/puff loops are skipped. Everything else (lights,
// wildlife, WBM/PVRZ setpieces, unclassified overlays) renders vanilla.
static void detour_static_render(void* thisPtr, void* area, void* vidMode) {
  try {
    if (g_ctx && g_ctx->cfg.enablePointEffects && g_ctx->cfg.enableWaterEffect &&
        probe::override_effect_replacement_enabled() && thisPtr) {
      game::ARE_Animation_st header{};
      const auto* headerAddress =
          reinterpret_cast<const std::byte*>(thisPtr) + offsetof(game::CGameStatic, m_header);
      if (core::safe_read(headerAddress, header) &&
          (header.nFlags &
           (game::kAreAnimationFlagUseWbm | game::kAreAnimationFlagUsePvrz)) == 0) {
        const auto info = game::make_area_animation_info(header);
        // Suppress only once the authored draw box is known: the engine's
        // first draw loads the BAM that box is read from, and the point then
        // lands exactly where RenderBam would have drawn.
        if (game::should_replace_animation_draw(info.resrefView(), info.kind) &&
            area::static_envelope_ready(thisPtr)) {
          return;  // replaced by the shader's point effects
        }
      }
    }
  } catch (...) {
    // Suppression is cosmetic; any doubt falls through to the engine draw.
  }
  g_staticRenderHook.original()(thisPtr, area, vidMode);
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

    if (ctx.addrs.StaticRender) {
      try {
        g_staticRenderHook.create(reinterpret_cast<void*>(ctx.addrs.StaticRender),
                                  reinterpret_cast<void*>(&detour_static_render));
        g_staticRenderHook.enable();
        LOG_INFO("CGameStatic::Render hook installed (fire/smoke BAM replacement)");
      } catch (const std::exception& e) {
        LOG_WARN("CGameStatic::Render hook failed ({}); authored fire/smoke draws stay vanilla",
                 e.what());
      } catch (...) {
        LOG_WARN("CGameStatic::Render hook failed; authored fire/smoke draws stay vanilla");
      }
    }

    if ((ctx.cfg.smoothSpriteMovement || ctx.cfg.interpolateAnimations) &&
        ctx.addrs.SpriteRender && ctx.addrs.SpriteRenderMarkers &&
        ctx.addrs.SpriteRenderHealthBar) {
      try {
        g_spriteRenderHook.create(reinterpret_cast<void*>(ctx.addrs.SpriteRender),
                                  reinterpret_cast<void*>(&detour_sprite_render));
        g_spriteMarkersHook.create(reinterpret_cast<void*>(ctx.addrs.SpriteRenderMarkers),
                                   reinterpret_cast<void*>(&detour_sprite_markers));
        g_spriteHealthBarHook.create(reinterpret_cast<void*>(ctx.addrs.SpriteRenderHealthBar),
                                     reinterpret_cast<void*>(&detour_sprite_health_bar));
        g_spriteRenderHook.enable();
        g_spriteMarkersHook.enable();
        g_spriteHealthBarHook.enable();
        LOG_INFO("Sprite render hooks installed (movement smoothing={})",
                 ctx.cfg.smoothSpriteMovement);
        if (ctx.cfg.interpolateAnimations && ctx.addrs.VidCellGetFrame &&
            ctx.addrs.VidCellGetCurrentCenterPoint && ctx.addrs.VidCellGetCurrentFrameSize) {
          try {
            wchar_t executablePath[MAX_PATH]{};
            const auto length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) throw std::runtime_error("no game path");
            const auto directory =
                std::filesystem::path(executablePath).parent_path() / "iee-interp";
            g_animationInterp.set_directory(directory);
            g_vidCellGetFrameHook.create(reinterpret_cast<void*>(ctx.addrs.VidCellGetFrame),
                                         reinterpret_cast<void*>(&detour_vid_cell_get_frame));
            g_vidCellCenterPointHook.create(
                reinterpret_cast<void*>(ctx.addrs.VidCellGetCurrentCenterPoint),
                reinterpret_cast<void*>(&detour_vid_cell_center_point));
            g_vidCellFrameSizeHook.create(
                reinterpret_cast<void*>(ctx.addrs.VidCellGetCurrentFrameSize),
                reinterpret_cast<void*>(&detour_vid_cell_frame_size));
            g_vidCellGetFrameHook.enable();
            g_vidCellCenterPointHook.enable();
            g_vidCellFrameSizeHook.enable();
            g_animationInterpActive = true;
            LOG_INFO("Animation interpolation hooks installed (expanded BAMs from {})",
                     directory.string());
          } catch (...) {
            g_animationInterpActive = false;
            (void)g_vidCellFrameSizeHook.remove();
            (void)g_vidCellCenterPointHook.remove();
            (void)g_vidCellGetFrameHook.remove();
            LOG_WARN("Animation interpolation hooks failed; animations play their original "
                     "frames");
          }
        }
      } catch (...) {
        (void)g_spriteHealthBarHook.remove();
        (void)g_spriteMarkersHook.remove();
        (void)g_spriteRenderHook.remove();
        LOG_WARN("Sprite movement smoothing hooks failed; creatures move at the logic rate");
      }
    }

    if (ctx.cfg.smoothSpriteMovement) {
      std::size_t installed = 0;
      for (std::size_t slot = 0; slot < g_objectRenderHooks.size(); ++slot) {
        const auto target = ctx.addrs.SmoothedObjectRenders[slot];
        if (!target) continue;
        try {
          g_objectRenderHooks[slot].create(reinterpret_cast<void*>(target),
                                           reinterpret_cast<void*>(kObjectRenderDetours[slot]));
          g_objectRenderHooks[slot].enable();
          ++installed;
        } catch (...) {
          (void)g_objectRenderHooks[slot].remove();
          LOG_WARN("Object movement smoothing hook {} failed; that object type moves at the "
                   "logic rate",
                   ctx.manifest->smoothedObjectRenders[slot].name);
        }
      }
      LOG_INFO("Object movement smoothing hooks installed: {}", installed);

      if (ctx.addrs.ParticleUpdate && ctx.addrs.ParticleRender) {
        try {
          g_particleUpdateHook.create(reinterpret_cast<void*>(ctx.addrs.ParticleUpdate),
                                      reinterpret_cast<void*>(&detour_particle_update));
          g_particleRenderHook.create(reinterpret_cast<void*>(ctx.addrs.ParticleRender),
                                      reinterpret_cast<void*>(&detour_particle_render));
          g_particleUpdateHook.enable();
          g_particleRenderHook.enable();
          LOG_INFO("Particle movement smoothing hooks installed (rain, snow, sparkles)");
        } catch (...) {
          (void)g_particleRenderHook.remove();
          (void)g_particleUpdateHook.remove();
          LOG_WARN("Particle movement smoothing hooks failed; particles move at the logic rate");
        }
      }
    }

    if (ctx.addrs.RenderFog && ctx.addrs.DrawFlush) {
      try {
        features::world_post_configure(ctx.cfg);
        g_drawFlush = reinterpret_cast<DrawFlushFn>(ctx.addrs.DrawFlush);
        g_renderFogHook.create(reinterpret_cast<void*>(ctx.addrs.RenderFog),
                               reinterpret_cast<void*>(&detour_render_fog));
        g_renderFogHook.enable();
        LOG_INFO("CInfinity::RenderFog hook installed (soft fog of war={}, bloom={})",
                 ctx.cfg.softFogOfWar, ctx.cfg.bloom);
        if (ctx.addrs.DrawQueueCount && ctx.addrs.DrawQueueCommands) {
          g_drawQueueCount = reinterpret_cast<std::int32_t*>(ctx.addrs.DrawQueueCount);
          g_drawQueueCommands = reinterpret_cast<game::DrawCommand*>(ctx.addrs.DrawQueueCommands);
          g_drawFlushHook.create(reinterpret_cast<void*>(ctx.addrs.DrawFlush),
                                 reinterpret_cast<void*>(&detour_draw_flush));
          g_drawFlushHook.enable();
          LOG_INFO("DrawFlush_GL hook installed (bloom from the engine's additive draws)");
          if (ctx.addrs.TextureUpload) {
            g_textureUploadHook.create(reinterpret_cast<void*>(ctx.addrs.TextureUpload),
                                       reinterpret_cast<void*>(&detour_texture_upload));
            g_textureUploadHook.enable();
            LOG_INFO("TexSubImage_GL hook installed at 0x{:X} (sprite upscaling={})",
                     ctx.addrs.TextureUpload, ctx.cfg.spriteUpscale);
          }
        }
      } catch (...) {
        (void)g_textureUploadHook.remove();
        g_drawQueueCount = nullptr;
        g_drawQueueCommands = nullptr;
        (void)g_drawFlushHook.remove();
        g_drawFlush = nullptr;
        (void)g_renderFogHook.remove();
        LOG_WARN("CInfinity::RenderFog hook failed; fog of war and bloom stay vanilla");
      }
    }

    g_loadAreaHook.enable();
    LOG_INFO("LoadArea hook enabled");

    g_renderTextureHook.enable();
    LOG_INFO("RenderTexture hook enabled");

    LOG_INFO("All hooks installed successfully");
    LOG_INFO("LoadArea: 0x{:X}", ctx.addrs.LoadArea);
    LOG_INFO("RenderTexture: 0x{:X}", ctx.addrs.RenderTexture);
    LOG_INFO("RenderTexture hook enabled - will detect upscaled textures automatically");

    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("Exception during hook installation: {}", e.what());
    (void)g_staticRenderHook.remove();
    (void)g_drawColorToneHook.remove();
    (void)g_renderTextureHook.remove();
    (void)g_loadAreaHook.remove();
    g_ctx = nullptr;
    delete g_hookInit;
    g_hookInit = nullptr;
    return false;
  } catch (...) {
    LOG_ERROR("Unknown exception during hook installation");
    (void)g_staticRenderHook.remove();
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

  (void)g_textureUploadHook.remove();
  (void)g_drawFlushHook.remove();
  g_drawQueueCount = nullptr;
  g_drawQueueCommands = nullptr;
  (void)g_renderFogHook.remove();
  g_drawFlush = nullptr;
  features::world_post_forget();
  g_animationInterpActive = false;
  (void)g_vidCellFrameSizeHook.remove();
  (void)g_vidCellCenterPointHook.remove();
  (void)g_vidCellGetFrameHook.remove();
  (void)g_particleRenderHook.remove();
  (void)g_particleUpdateHook.remove();
  for (auto& hook : g_objectRenderHooks) (void)hook.remove();
  (void)g_spriteHealthBarHook.remove();
  (void)g_spriteMarkersHook.remove();
  (void)g_spriteRenderHook.remove();
  (void)g_staticRenderHook.remove();
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
  // Quiesce engine entry points before dependent frame/GL hooks and shared
  // state are torn down. MinHook itself stays initialized until
  // uninstall_all(), after every MinHook-backed subsystem has removed its
  // hooks.
  (void)g_textureUploadHook.disable();
  (void)g_drawFlushHook.disable();
  (void)g_renderFogHook.disable();
  (void)g_vidCellFrameSizeHook.disable();
  (void)g_vidCellCenterPointHook.disable();
  (void)g_vidCellGetFrameHook.disable();
  (void)g_particleRenderHook.disable();
  (void)g_particleUpdateHook.disable();
  for (auto& hook : g_objectRenderHooks) (void)hook.disable();
  (void)g_spriteHealthBarHook.disable();
  (void)g_spriteMarkersHook.disable();
  (void)g_spriteRenderHook.disable();
  (void)g_staticRenderHook.disable();
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

void retry_shader_probe_install() noexcept {
  try {
    if (g_ctx) install_shader_probes_once();
  } catch (...) {
    // A frame boundary must never depend on optional shader-probe setup.
  }
}
}  // namespace iee::hooks
