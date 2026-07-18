#include "area_state.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_context.h"
#include "iee/core/gl_state_guard.h"
#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"
#include "iee/game/are_animations.h"
#include "iee/game/area_texture.h"
#include "iee/game/object_statics.h"
#include "iee/game/opengl_types.h"
#include "iee/game/resref_runtime.h"
#include "iee/game/texture_units.h"
#include "iee/game/tile_liquid.h"
#include "iee/game/tis_palette.h"
#include "iee/game/wed_runtime.h"
#include "iee/shader_probe.h"

namespace iee::area {
namespace {
bool same_resref(std::string_view lhs, const game::ResrefBuffer& rhs) noexcept {
  return lhs == game::resref_view(rhs);
}

struct AreaGpuSnapshot {
  game::AreaCellTexture texture;
  std::array<float, 3> waterTint{0.5f, 0.5f, 0.5f};
  std::uint64_t generation{};
};

std::mutex g_areaGpuMutex;
std::shared_ptr<const AreaGpuSnapshot> g_latestAreaGpu;
std::atomic<std::uint64_t> g_nextAreaGpuGeneration{1};
std::mutex g_areaRefreshCommitMutex;
std::atomic<std::uint64_t> g_areaRefreshGeneration{0};

// Render-thread-owned GL state.
unsigned g_areaTexture{};
HGLRC g_areaTextureContext{};
std::uint64_t g_uploadedAreaGeneration{};
HGLRC g_failedAreaTextureContext{};
std::uint64_t g_failedAreaGeneration{};
constexpr float kBaseCellWorldPixels = 64.0f;
constexpr std::size_t kMaxTintTileSamples = 32;
constexpr std::size_t kMaxTintTileAttempts = 256;

void queue_area_gpu_snapshot(game::AreaCellTexture texture, std::array<float, 3> waterTint) {
  auto snapshot = std::make_shared<AreaGpuSnapshot>();
  snapshot->texture = std::move(texture);
  snapshot->waterTint = waterTint;
  std::lock_guard lock(g_areaGpuMutex);
  snapshot->generation = g_nextAreaGpuGeneration.fetch_add(1, std::memory_order_relaxed);
  g_latestAreaGpu = std::move(snapshot);
}

void queue_no_liquid_snapshot() {
  game::AreaCellTexture noLiquid{.width = 1, .height = 1, .texels = {0}};
  queue_area_gpu_snapshot(std::move(noLiquid), {0.5f, 0.5f, 0.5f});
}

constexpr std::size_t kMaxUnclassifiedResrefsLogged = 12;

// Per-area log dedupe across the LoadArea and render-thread refresh paths.
std::mutex g_areAnimationsLogMutex;
game::ResrefBuffer g_lastLoggedAreResref{};

bool read_area_resref(const game::CGameArea* area, game::ResrefBuffer& out) {
  game::CResRef runtimeResref{};
  const auto* resrefAddress =
      reinterpret_cast<const std::byte*>(area) + offsetof(game::CGameArea, m_resref);
  return core::safe_read(resrefAddress, runtimeResref) &&
         game::read_runtime_resref(runtimeResref.m_resRef.data(), out);
}

void log_area_animation_summary(const game::AreaAnimationsInfo& info) {
  std::size_t shown = 0;
  for (const auto& animation : info.animations) {
    if (animation.isShown()) ++shown;
  }
  LOG_INFO(
      "ARE animations {}: total={}, shown={}, fire={}, smoke={}, fountain={}, light={}, "
      "water={}, lava={}, wildlife={}, unclassified={}",
      info.areaResrefView(), info.animations.size(), shown,
      info.count_of(game::AreaAnimationKind::Fire), info.count_of(game::AreaAnimationKind::Smoke),
      info.count_of(game::AreaAnimationKind::Fountain),
      info.count_of(game::AreaAnimationKind::Light), info.count_of(game::AreaAnimationKind::Water),
      info.count_of(game::AreaAnimationKind::Lava),
      info.count_of(game::AreaAnimationKind::Wildlife),
      info.count_of(game::AreaAnimationKind::None));

  std::string unclassified;
  std::size_t unclassifiedListed = 0;
  for (const auto& animation : info.animations) {
    if (animation.kind != game::AreaAnimationKind::None) {
      LOG_DEBUG("ARE animation {}: kind={}, resref={}, name=\"{}\", pos=({}, {}), shown={}, "
                "lightSource={}",
                info.areaResrefView(), game::area_animation_kind_name(animation.kind),
                animation.resrefView(), animation.nameView(), animation.x, animation.y,
                animation.isShown(), animation.isLightSource());
      continue;
    }
    if (unclassifiedListed < kMaxUnclassifiedResrefsLogged && !animation.resrefView().empty() &&
        unclassified.find(animation.resrefView()) == std::string::npos) {
      if (!unclassified.empty()) unclassified += ", ";
      unclassified += animation.resrefView();
      ++unclassifiedListed;
    }
  }
  if (!unclassified.empty()) {
    // The visibility that grows the classification table from real data.
    LOG_INFO("ARE unclassified animation resrefs for {}: {}", info.areaResrefView(), unclassified);
  }
}

// Resolves the CGameObjectArray globals once per process from the manifest's
// GetShare pattern. Any ambiguity fails closed and latches, disabling the
// scan for the session.
const game::ObjectArrayGlobals& resolved_object_array(const game::BuildManifest& manifest) {
  static const game::ObjectArrayGlobals globals = [&manifest] {
    game::ObjectArrayGlobals resolved{};
    if (manifest.patterns.objectArrayGetShare.empty()) {
      return resolved;
    }
    std::size_t matchCount = 0;
    auto* function = core::find_unique_in_module(
        nullptr, manifest.patterns.objectArrayGetShare, &matchCount);
    if (!function) {
      LOG_WARN("ARE animation scan disabled: GetShare pattern matched {} times", matchCount);
      return resolved;
    }
    if (!game::decode_object_array_globals(static_cast<const std::byte*>(function), 0x60,
                                           resolved)) {
      LOG_WARN("ARE animation scan disabled: GetShare RIP operands did not decode");
      resolved = {};
      return resolved;
    }
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    LOG_INFO("CGameObjectArray resolved: GetShare RVA=0x{:X} (reference 0x{:X}), entries RVA=0x{:X}, "
             "maxIndex RVA=0x{:X}",
             reinterpret_cast<std::uintptr_t>(function) - moduleBase,
             manifest.referenceRvas.objectArrayGetShare,
             reinterpret_cast<std::uintptr_t>(resolved.entries) - moduleBase,
             reinterpret_cast<std::uintptr_t>(resolved.maxArrayIndex) - moduleBase);
    return resolved;
  }();
  return globals;
}

// Collects and classifies the active area's authored ARE ambient animations
// from the live CGameStatic objects in the engine's object array — fresh on
// every refresh (cheap, reflects script toggles and save state). Publication
// reuses the WED refresh generation so a stale result can never overwrite a
// newer area's snapshot.
void refresh_area_animations(AppContext& ctx, const game::CGameArea* area,
                             std::uint64_t refreshGeneration) noexcept {
  if (!ctx.cfg.enableAreaAnimationScan || !area) {
    return;
  }
  try {
    game::ResrefBuffer areaResref{};
    if (!read_area_resref(area, areaResref) || game::resref_view(areaResref).empty()) {
      LOG_DEBUG("ARE animation scan: active area resref unavailable");
      return;
    }

    const auto& objectArray = resolved_object_array(*ctx.manifest);
    if (!objectArray.valid()) {
      return;  // Already logged once at resolution time.
    }

    game::AreaAnimationsInfo info{};
    if (!game::collect_area_static_animations(objectArray, area, info)) {
      LOG_DEBUG("ARE animation walk failed for {}", game::resref_view(areaResref));
      return;
    }
    info.areaResref = areaResref;
    auto snapshot = std::make_shared<const game::AreaAnimationsInfo>(std::move(info));

    // Pack the shader point set outside the commit lock; publish it together
    // with the snapshot under the same generation gate.
    std::vector<game::AreaEffectPoint> effectPoints;
    if (ctx.cfg.enablePointEffects) {
      effectPoints = game::build_area_effect_points(*snapshot);
    }

    {
      std::lock_guard commitLock(g_areaRefreshCommitMutex);
      if (g_areaRefreshGeneration.load(std::memory_order_acquire) != refreshGeneration) {
        LOG_DEBUG("Discarding stale ARE animation refresh generation {}", refreshGeneration);
        return;
      }
      ctx.areaAnimations.store(snapshot);
      static_assert(sizeof(game::AreaEffectPoint) == 8 * sizeof(float));
      probe::set_area_effect_points(
          effectPoints.empty() ? nullptr : &effectPoints.front().x, effectPoints.size());
    }

    // The walk reruns every refresh; log the summary once per area resref.
    bool logSummary = false;
    {
      std::lock_guard logLock(g_areAnimationsLogMutex);
      logSummary = g_lastLoggedAreResref != areaResref;
      if (logSummary) g_lastLoggedAreResref = areaResref;
    }
    if (logSummary) {
      log_area_animation_summary(*snapshot);
      LOG_INFO("ARE effect points published: {} (pointEffects={})", effectPoints.size(),
               ctx.cfg.enablePointEffects);
    }
  } catch (const std::exception& e) {
    LOG_ERROR("ARE animation refresh failed: {}", e.what());
  } catch (...) {
    LOG_ERROR("ARE animation refresh failed with an unknown exception");
  }
}

const game::CGameArea* read_loaded_area_candidate(const game::CGameArea* candidate) {
  if (!candidate) {
    return nullptr;
  }

  std::uint8_t loaded = 0;
  const auto* loadedAddress =
      reinterpret_cast<const std::byte*>(candidate) + offsetof(game::CGameArea, m_bAreaLoaded);
  if (!core::safe_read(loadedAddress, loaded) || loaded == 0) {
    return nullptr;
  }

  return candidate;
}
}  // namespace

const game::CGameArea* resolve_active_area(void* infGame, const game::BuildManifest& manifest) {
  if (!infGame) {
    return nullptr;
  }

  const auto* gameBytes = reinterpret_cast<const std::byte*>(infGame);

  std::uint8_t visibleArea = 0;
  std::array<game::CGameArea*, 12> areas{};
  if (!core::safe_read(gameBytes + manifest.offsets.infGameVisibleArea, visibleArea) ||
      !core::safe_read(gameBytes + manifest.offsets.infGameAreas, areas) ||
      visibleArea >= areas.size()) {
    visibleArea = 0xFF;
  } else if (const auto* visible = read_loaded_area_candidate(areas[visibleArea])) {
    return visible;
  }

  game::CGameArea* masterArea = nullptr;
  if (core::safe_read(gameBytes + manifest.offsets.infGameAreaMaster, masterArea)) {
    if (const auto* master = read_loaded_area_candidate(masterArea)) {
      return master;
    }
  }

  if (visibleArea != 0xFF) {
    for (const auto* area : areas) {
      if (const auto* loaded = read_loaded_area_candidate(area)) {
        return loaded;
      }
    }
  }
  return nullptr;
}

bool read_view_transform(const game::CGameArea* area, ViewTransform& out) {
  if (!area) {
    return false;
  }
  const auto* infinityBase =
      reinterpret_cast<const std::byte*>(area) + offsetof(game::CGameArea, m_cInfinity);

  std::int32_t newX = 0;
  std::int32_t newY = 0;
  game::CRect viewPortNotZoomed{};
  game::CRect viewPort{};
  if (!core::safe_read(infinityBase + offsetof(game::CInfinity, nNewX), newX) ||
      !core::safe_read(infinityBase + offsetof(game::CInfinity, nNewY), newY) ||
      !core::safe_read(infinityBase + offsetof(game::CInfinity, rViewPortNotZoomed),
                       viewPortNotZoomed) ||
      !core::safe_read(infinityBase + offsetof(game::CInfinity, rViewPort), viewPort)) {
    return false;
  }

  const float logicalW = static_cast<float>(viewPortNotZoomed.right - viewPortNotZoomed.left);
  const float logicalH = static_cast<float>(viewPortNotZoomed.bottom - viewPortNotZoomed.top);
  const float worldW = static_cast<float>(viewPort.right - viewPort.left);
  const float worldH = static_cast<float>(viewPort.bottom - viewPort.top);
  if (logicalW <= 0.0f || logicalH <= 0.0f || worldW <= 0.0f || worldH <= 0.0f) {
    return false;
  }

  // ScreenToWorld in logical px: world = nNew + (logical - rVPNZ.origin) * rVP.size/rVPNZ.size
  // The rVPNZ.origin shift folds into scroll (in world px); the per-pixel
  // term is converted to PHYSICAL pixels by the probe at feed time.
  out.viewWorldW = worldW;
  out.viewWorldH = worldH;
  out.scrollX =
      static_cast<float>(newX) - static_cast<float>(viewPortNotZoomed.left) * worldW / logicalW;
  out.scrollY =
      static_cast<float>(newY) - static_cast<float>(viewPortNotZoomed.top) * worldH / logicalH;

  // Debug: raw inputs, rate-limited — pairs with F10 snapshots so the
  // transform arithmetic can be checked exactly against screenshots.
  static std::uint32_t s_lastLogTick = 0;
  const std::uint32_t nowTick = GetTickCount();
  if (nowTick - s_lastLogTick > 5000) {
    s_lastLogTick = nowTick;
    LOG_DEBUG(
        "View transform: nNew=({}, {}), worldViewport=({}, {}, {}, {}), "
        "logicalViewport=({}, {}, {}, {}), scroll=({}, {}), viewWorld={}x{}",
        newX, newY, viewPort.left, viewPort.top, viewPort.right, viewPort.bottom,
        viewPortNotZoomed.left, viewPortNotZoomed.top, viewPortNotZoomed.right,
        viewPortNotZoomed.bottom, out.scrollX, out.scrollY, out.viewWorldW, out.viewWorldH);
  }
  return true;
}

void refresh_wed_cache(AppContext& ctx, void* infGame) {
  std::uint64_t refreshGeneration = 0;
  {
    // Start and commit use the same short critical section. If load/render
    // callbacks overlap, an older parse can never publish after a newer one.
    std::lock_guard commitLock(g_areaRefreshCommitMutex);
    refreshGeneration = g_areaRefreshGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    queue_no_liquid_snapshot();
    probe::set_area_effect_points(nullptr, 0);
    ctx.activeArea.store(nullptr);
    ctx.wed.store(std::shared_ptr<const game::WedAreaInfo>{});
  }

  const auto resetIfCurrent = [&]() noexcept {
    try {
      std::lock_guard commitLock(g_areaRefreshCommitMutex);
      if (g_areaRefreshGeneration.load(std::memory_order_acquire) != refreshGeneration) return;
      queue_no_liquid_snapshot();
      ctx.activeArea.store(nullptr);
      ctx.wed.store(std::shared_ptr<const game::WedAreaInfo>{});
    } catch (...) {
      // Preserve the previous immutable snapshot on allocation failure.
    }
  };

  try {
    const auto* area = resolve_active_area(infGame, *ctx.manifest);
    if (!area) {
      LOG_DEBUG("LoadArea: could not resolve active CGameArea for WED caching");
      return;
    }

    // Independent of WED parsing: a WED failure must not cost the authored
    // ARE animation classification, and vice versa.
    refresh_area_animations(ctx, area, refreshGeneration);

    const auto* areaBytes = reinterpret_cast<const std::byte*>(area);
    game::CResWED* wedPointer = nullptr;
    if (!core::safe_read(areaBytes + offsetof(game::CGameArea, m_pResWED), wedPointer) ||
        !wedPointer) {
      LOG_DEBUG("LoadArea: active area has no readable WED resource");
      return;
    }

    if (ctx.draw.CRes_Demand) {
      try {
        (void)ctx.draw.CRes_Demand(wedPointer);
      } catch (...) {
        LOG_WARN("LoadArea: CRes_Demand threw while demanding WED");
      }
    }

    game::CResWED wedResource{};
    if (!core::safe_read(wedPointer, wedResource)) {
      LOG_WARN("LoadArea: failed to read CResWED");
      return;
    }

    game::WedAreaInfo wed{};
    if (!game::parse_loaded_wed(wedResource.baseclass_0, wed)) {
      game::ResrefBuffer areaResref{};
      game::CResRef runtimeAreaResref{};
      if (core::safe_read(areaBytes + offsetof(game::CGameArea, m_resref), runtimeAreaResref)) {
        (void)game::read_runtime_resref(runtimeAreaResref.m_resRef.data(), areaResref);
      }
      LOG_WARN("LoadArea: failed to parse loaded WED for area {}", game::resref_view(areaResref));
      return;
    }

    const auto liquidMask = game::liquid_overlay_mask(wed);
    auto cachedWed = std::make_shared<const game::WedAreaInfo>(std::move(wed));

    auto packed = game::pack_area_liquid_texture(*cachedWed);
    if (!packed) {
      LOG_WARN("Area liquid mask build failed; keeping the queued no-liquid generation");
      return;
    }

    // Sample a bounded set of authored liquid tiles for area-specific tint.
    // Goo/sewage/swamp need their own authored identity too; lava is excluded
    // because the shader deliberately gives it a fixed emissive grade.
    // LoadArea prepares CPU data only; the render thread owns every GL upload.
    double tintSum[3] = {0.0, 0.0, 0.0};
    std::size_t tintTiles = 0;
    std::size_t tintOpaquePixels = 0;
    std::array<game::CInfTileSet*, 5> tileSets{};
    const auto* tileSetsAddress =
        areaBytes + offsetof(game::CGameArea, m_cInfinity) + offsetof(game::CInfinity, pTileSets);
    (void)core::safe_read(tileSetsAddress, tileSets);
    std::size_t tintAttempts = 0;
    const auto sampleTint = [&](std::size_t overlayIndex, std::uint16_t tileIndex) {
      if (overlayIndex >= tileSets.size() || !tileSets[overlayIndex]) return false;

      void** tileResources = nullptr;
      std::uint32_t tileCount = 0;
      const auto* tileSetBytes = reinterpret_cast<const std::byte*>(tileSets[overlayIndex]);
      if (!core::safe_read(tileSetBytes + offsetof(game::CInfTileSet, pResTiles), tileResources) ||
          !core::safe_read(tileSetBytes + offsetof(game::CInfTileSet, nTiles), tileCount) ||
          !tileResources || tileIndex >= tileCount) {
        return false;
      }

      void* wrapper = nullptr;
      void* tileResPtr = nullptr;
      if (!core::safe_read(tileResources + tileIndex, wrapper) || !wrapper ||
          !core::safe_read(wrapper, tileResPtr) || !tileResPtr) {
        return false;
      }

      game::CRes tileRes{};
      if (!core::safe_read(tileResPtr, tileRes) || !tileRes.bLoaded || !tileRes.pData ||
          tileRes.nSize < game::kPaletteTileBytes) {
        return false;
      }

      // Work from one stable local snapshot. is_readable() proves page access,
      // not object lifetime, and repeatedly traversing mutable engine memory
      // while calculating a palette average would widen that race window.
      std::array<std::uint8_t, game::kPaletteTileBytes> tileBytes{};
      if (!core::safe_read(tileRes.pData, tileBytes)) return false;
      if (const auto average =
              game::palette_tile_average_color(tileBytes.data(), tileBytes.size())) {
        const auto weight = static_cast<double>(average->opaquePixelCount);
        tintSum[0] += average->linearRgb[0] * weight;
        tintSum[1] += average->linearRgb[1] * weight;
        tintSum[2] += average->linearRgb[2] * weight;
        tintOpaquePixels += average->opaquePixelCount;
        ++tintTiles;
        return true;
      }
      return false;
    };

    for (std::size_t overlayIndex = 1;
         overlayIndex < cachedWed->overlays.size() && overlayIndex <= 7 &&
         tintTiles < kMaxTintTileSamples && tintAttempts < kMaxTintTileAttempts;
         ++overlayIndex) {
      const auto& overlay = cachedWed->overlays[overlayIndex];
      if (overlay.liquidMode == game::TileLiquidMode::None ||
          overlay.liquidMode == game::TileLiquidMode::Lava) {
        continue;
      }
      for (const auto tileIndex : overlay.tintTileCandidates) {
        ++tintAttempts;
        (void)sampleTint(overlayIndex, tileIndex);
        if (tintTiles >= kMaxTintTileSamples || tintAttempts >= kMaxTintTileAttempts) break;
      }
    }

    std::array<float, 3> waterTint{0.5f, 0.5f, 0.5f};
    if (tintOpaquePixels > 0) {
      const auto inverse = 1.0 / static_cast<double>(tintOpaquePixels);
      waterTint = {static_cast<float>(tintSum[0] * inverse),
                   static_cast<float>(tintSum[1] * inverse),
                   static_cast<float>(tintSum[2] * inverse)};
    }

    game::CResWED* currentWedPointer = nullptr;
    if (resolve_active_area(infGame, *ctx.manifest) != area || !read_loaded_area_candidate(area) ||
        !core::safe_read(areaBytes + offsetof(game::CGameArea, m_pResWED), currentWedPointer) ||
        currentWedPointer != wedPointer) {
      LOG_DEBUG("Discarding WED refresh because the active area resource changed during parsing");
      return;
    }

    std::lock_guard commitLock(g_areaRefreshCommitMutex);
    if (g_areaRefreshGeneration.load(std::memory_order_acquire) != refreshGeneration) {
      LOG_DEBUG("Discarding stale WED refresh generation {}", refreshGeneration);
      return;
    }

    const int packedWidth = packed->width;
    const int packedHeight = packed->height;
    queue_area_gpu_snapshot(std::move(*packed), waterTint);
    ctx.activeArea.store(area);
    ctx.wed.store(cachedWed);
    if (!same_resref(cachedWed->areaResrefView(), ctx.lastLoggedWedArea)) {
      LOG_INFO("Loaded WED {}: overlays={}, base={}x{}, liquidOverlayMask=0x{:02X}",
               cachedWed->areaResrefView(), cachedWed->overlayCount, cachedWed->baseWidth,
               cachedWed->baseHeight, liquidMask);
      for (std::size_t i = 1; i < cachedWed->overlays.size() && i <= 7; ++i) {
        const auto& overlay = cachedWed->overlays[i];
        if (overlay.tilesetResrefView().empty()) continue;
        LOG_DEBUG("WED overlay {}: tileset={}, mode={}, cells={}", i, overlay.tilesetResrefView(),
                  game::tile_liquid_mode_name(overlay.liquidMode), overlay.coverageCells);
      }
      ctx.lastLoggedWedArea = cachedWed->areaResref;
    } else {
      LOG_DEBUG("Reloaded WED {}: overlays={}, base={}x{}, liquidOverlayMask=0x{:02X}",
                cachedWed->areaResrefView(), cachedWed->overlayCount, cachedWed->baseWidth,
                cachedWed->baseHeight, liquidMask);
    }
    if (tintOpaquePixels > 0) {
      LOG_INFO(
          "Area liquid tint: ({:.3f}, {:.3f}, {:.3f}) from {} opaque pixels across {} authored "
          "tiles",
          waterTint[0], waterTint[1], waterTint[2], tintOpaquePixels, tintTiles);
    }
    LOG_INFO("Area liquid mask prepared: {}x{} cells, {} tint candidates tried", packedWidth,
             packedHeight, tintAttempts);
  } catch (const std::exception& e) {
    LOG_ERROR("Area WED refresh failed: {}", e.what());
    resetIfCurrent();
  } catch (...) {
    LOG_ERROR("Area WED refresh failed with an unknown exception");
    resetIfCurrent();
  }
}

void reset_gpu_area_state() noexcept {
  try {
    std::lock_guard commitLock(g_areaRefreshCommitMutex);
    g_areaRefreshGeneration.fetch_add(1, std::memory_order_acq_rel);
    queue_no_liquid_snapshot();
    probe::set_area_effect_points(nullptr, 0);
  } catch (...) {
    // The previous immutable GPU snapshot remains valid until another
    // transition or refresh can publish a complete generation.
  }
}

bool flush_pending_gpu_upload() noexcept {
  try {
    std::shared_ptr<const AreaGpuSnapshot> snapshot;
    {
      std::lock_guard lock(g_areaGpuMutex);
      snapshot = g_latestAreaGpu;
    }
    if (!snapshot || snapshot->texture.width <= 0 || snapshot->texture.height <= 0 ||
        snapshot->texture.texels.empty())
      return false;

    auto& gl = game::gl::get_gl_functions();
    const auto context = game::gl::current_context();
    if (!context || !gl.textureUploadAvailable) return false;

    if (context != g_areaTextureContext) {
      g_areaTextureContext = context;
      g_areaTexture = 0;
      g_uploadedAreaGeneration = 0;
      g_failedAreaTextureContext = nullptr;
      g_failedAreaGeneration = 0;
    }
    if (g_areaTexture != 0 && g_uploadedAreaGeneration == snapshot->generation) return true;
    if (context == g_failedAreaTextureContext && snapshot->generation == g_failedAreaGeneration) {
      return false;
    }

    const auto blockFailedGeneration = [&] {
      g_failedAreaTextureContext = context;
      g_failedAreaGeneration = snapshot->generation;
      return false;
    };

    core::GlStateGuard guard({game::texture_units::AreaMask});
    // The engine may leave an unrelated error pending. Do not attribute it to
    // this upload and latch the area generation as failed for the context.
    game::gl::discard_errors();
    if (g_areaTexture == 0) gl.glGenTextures(1, &g_areaTexture);
    gl.glActiveTexture(game::gl::TEXTURE0 + game::texture_units::AreaMask);
    gl.glBindTexture(game::gl::TEXTURE_2D, g_areaTexture);
    gl.glPixelStorei(game::gl::UNPACK_ALIGNMENT, 1);
    gl.glTexImage2D(game::gl::TEXTURE_2D, 0, game::gl::R8, snapshot->texture.width,
                    snapshot->texture.height, 0, game::gl::RED, game::gl::UNSIGNED_BYTE,
                    snapshot->texture.texels.data());
    gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MIN_FILTER, game::gl::NEAREST);
    gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MAG_FILTER, game::gl::NEAREST);
    gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_S, game::gl::CLAMP_TO_EDGE);
    gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_T, game::gl::CLAMP_TO_EDGE);
    if (!game::gl::check_error("area liquid texture upload")) return blockFailedGeneration();

    g_uploadedAreaGeneration = snapshot->generation;
    g_failedAreaTextureContext = nullptr;
    g_failedAreaGeneration = 0;
    probe::set_area_water_tint(snapshot->waterTint[0], snapshot->waterTint[1],
                               snapshot->waterTint[2]);
    probe::set_area_world_size(static_cast<float>(snapshot->texture.width) * kBaseCellWorldPixels,
                               static_cast<float>(snapshot->texture.height) * kBaseCellWorldPixels);
    LOG_INFO("Area liquid mask uploaded: {}x{} cells (unit {}, texture {}, generation {})",
             snapshot->texture.width, snapshot->texture.height, game::texture_units::AreaMask,
             g_areaTexture, snapshot->generation);
    return true;
  } catch (...) {
    return false;
  }
}

bool bind_area_texture() noexcept {
  if (!flush_pending_gpu_upload() || g_areaTexture == 0) return false;
  auto& gl = game::gl::get_gl_functions();
  if (!gl.glActiveTexture || !gl.glBindTexture) return false;
  int previousActiveTexture = -1;
  if (gl.glGetIntegerv) gl.glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &previousActiveTexture);
  gl.glActiveTexture(game::gl::TEXTURE0 + game::texture_units::AreaMask);
  gl.glBindTexture(game::gl::TEXTURE_2D, g_areaTexture);
  if (previousActiveTexture >= 0) gl.glActiveTexture(static_cast<unsigned>(previousActiveTexture));
  return true;
}

void release_gpu_area_resources() noexcept {
  try {
    auto& gl = game::gl::get_gl_functions();
    if (g_areaTexture && g_areaTextureContext &&
        game::gl::current_context() == g_areaTextureContext && gl.glDeleteTextures) {
      gl.glDeleteTextures(1, &g_areaTexture);
    }
    g_areaTexture = 0;
    g_areaTextureContext = nullptr;
    g_uploadedAreaGeneration = 0;
    g_failedAreaTextureContext = nullptr;
    g_failedAreaGeneration = 0;
    std::lock_guard lock(g_areaGpuMutex);
    g_latestAreaGpu.reset();
  } catch (...) {
    // Explicit shutdown must not escape into EEex.
  }
}
}  // namespace iee::area
