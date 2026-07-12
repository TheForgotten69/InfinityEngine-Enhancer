#include "area_state.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "app_context.h"
#include "iee/core/gl_state_guard.h"
#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"
#include "iee/game/area_texture.h"
#include "iee/game/opengl_types.h"
#include "iee/game/resref_runtime.h"
#include "iee/game/tile_liquid.h"
#include "iee/game/wed_runtime.h"
#include "iee/shader_probe.h"

namespace iee::area {
    namespace {
        bool same_resref(std::string_view lhs, const game::ResrefBuffer &rhs) noexcept {
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

        // Render-thread-owned GL state.
        unsigned g_areaTexture{};
        HGLRC g_areaTextureContext{};
        std::uint64_t g_uploadedAreaGeneration{};

        void queue_area_gpu_snapshot(game::AreaCellTexture texture,
                                     std::array<float, 3> waterTint) {
            auto snapshot = std::make_shared<AreaGpuSnapshot>();
            snapshot->texture = std::move(texture);
            snapshot->waterTint = waterTint;
            snapshot->generation = g_nextAreaGpuGeneration.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(g_areaGpuMutex);
            g_latestAreaGpu = std::move(snapshot);
        }

        const game::CGameArea *read_loaded_area_candidate(const game::CGameArea *candidate) {
            if (!candidate) {
                return nullptr;
            }

            game::CGameArea snapshot{};
            if (!core::safe_read(candidate, snapshot) || !snapshot.m_bAreaLoaded) {
                return nullptr;
            }

            return candidate;
        }
    }

    const game::CGameArea *resolve_active_area(void *infGame, const game::BuildManifest &manifest) {
        if (!infGame) {
            return nullptr;
        }

        const auto *gameBytes = reinterpret_cast<const std::byte *>(infGame);

        std::uint8_t visibleArea = 0;
        std::array<game::CGameArea *, 12> areas{};
        if (!core::safe_read(gameBytes + manifest.offsets.infGameVisibleArea, visibleArea) ||
            !core::safe_read(gameBytes + manifest.offsets.infGameAreas, areas) ||
            visibleArea >= areas.size()) {
            visibleArea = 0xFF;
        } else if (const auto *visible = read_loaded_area_candidate(areas[visibleArea])) {
            return visible;
        }

        game::CGameArea *masterArea = nullptr;
        if (core::safe_read(gameBytes + manifest.offsets.infGameAreaMaster, masterArea)) {
            if (const auto *master = read_loaded_area_candidate(masterArea)) {
                return master;
            }
        }

        if (visibleArea != 0xFF) {
            for (const auto *area: areas) {
                if (const auto *loaded = read_loaded_area_candidate(area)) {
                    return loaded;
                }
            }
        }
        return nullptr;
    }

    bool read_area_scroll(const game::CGameArea *area, int &outOffsetX, int &outOffsetY) {
        if (!area) {
            return false;
        }

        // nNewX/nNewY is the view's world position in plain pixels — taken
        // straight from the decompiled CInfinity::GetWorldCoordinates:
        //   world = (nNew - rViewPort.origin) + screen
        // (m_ptCurrentPosExact is the same position in x10000 fixed point,
        // per the decompiled CInfinity::Scroll; not needed here.)
        const auto *base = reinterpret_cast<const std::byte *>(area);
        const auto *posXAddr = base + offsetof(game::CGameArea, m_cInfinity) +
                               offsetof(game::CInfinity, nNewX);
        const auto *posYAddr = base + offsetof(game::CGameArea, m_cInfinity) +
                               offsetof(game::CInfinity, nNewY);
        return core::safe_read(posXAddr, outOffsetX) && core::safe_read(posYAddr, outOffsetY);
    }

    bool read_area_zoom(const game::CGameArea *area, const game::BuildManifest &manifest, float &outZoom) {
        if (!area) {
            return false;
        }
        const auto *base = reinterpret_cast<const std::byte *>(area);
        const auto *zoomAddr = base + offsetof(game::CGameArea, m_cInfinity) + manifest.offsets.infinityZoom;
        return core::safe_read(zoomAddr, outZoom);
    }

    bool read_view_transform(const game::CGameArea *area, ViewTransform &out) {
        if (!area) {
            return false;
        }
        const auto *infinityBase = reinterpret_cast<const std::byte *>(area) +
                                   offsetof(game::CGameArea, m_cInfinity);

        std::int32_t newX = 0;
        std::int32_t newY = 0;
        game::CRect viewPortNotZoomed{};
        game::CRect viewPort{};
        if (!core::safe_read(infinityBase + offsetof(game::CInfinity, nNewX), newX) ||
            !core::safe_read(infinityBase + offsetof(game::CInfinity, nNewY), newY) ||
            !core::safe_read(infinityBase + offsetof(game::CInfinity, rViewPortNotZoomed), viewPortNotZoomed) ||
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
        out.scrollX = static_cast<float>(newX) - static_cast<float>(viewPortNotZoomed.left) * worldW / logicalW;
        out.scrollY = static_cast<float>(newY) - static_cast<float>(viewPortNotZoomed.top) * worldH / logicalH;

        // Debug: raw inputs, rate-limited — pairs with F10 snapshots so the
        // transform arithmetic can be checked exactly against screenshots.
        static std::uint32_t s_lastLogTick = 0;
        const std::uint32_t nowTick = GetTickCount();
        if (nowTick - s_lastLogTick > 5000) {
            s_lastLogTick = nowTick;
            LOG_INFO("ViewXform raw: nNew=({}, {}) rVP=({}, {}, {}, {}) rVPNZ=({}, {}, {}, {}) -> scroll=({}, {}) "
                     "viewWorld={}x{}",
                     newX, newY, viewPort.left, viewPort.top, viewPort.right, viewPort.bottom, viewPortNotZoomed.left,
                     viewPortNotZoomed.top, viewPortNotZoomed.right, viewPortNotZoomed.bottom, out.scrollX, out.scrollY,
                     out.viewWorldW, out.viewWorldH);
        }
        return true;
    }

    void refresh_wed_cache(AppContext &ctx, void *infGame) {
        reset_gpu_area_state();
        ctx.activeArea.store(nullptr);
        ctx.wed.store(std::shared_ptr<const game::WedAreaInfo>{});

        try {
            const auto *area = resolve_active_area(infGame, *ctx.manifest);
            if (!area) {
                LOG_DEBUG("LoadArea: could not resolve active CGameArea for WED caching");
                return;
            }

            game::CGameArea areaSnapshot{};
            if (!core::safe_read(area, areaSnapshot) || !areaSnapshot.m_pResWED) {
                LOG_DEBUG("LoadArea: active area has no readable WED resource");
                return;
            }

            if (ctx.draw.CRes_Demand) {
                try {
                    (void)ctx.draw.CRes_Demand(areaSnapshot.m_pResWED);
                } catch (...) {
                    LOG_WARN("LoadArea: CRes_Demand threw while demanding WED");
                }
            }

            game::CResWED wedResource{};
            if (!core::safe_read(areaSnapshot.m_pResWED, wedResource)) {
                LOG_WARN("LoadArea: failed to read CResWED");
                return;
            }

            game::WedAreaInfo wed{};
            if (!game::parse_loaded_wed(wedResource.baseclass_0, wed)) {
                game::ResrefBuffer areaResref{};
                (void)game::read_runtime_resref(areaSnapshot.m_resref.m_resRef.data(), areaResref);
                LOG_WARN("LoadArea: failed to parse loaded WED for area {}", game::resref_view(areaResref));
                return;
            }

            const auto liquidMask = game::liquid_overlay_mask(wed);
            auto wedSnapshot = std::make_shared<const game::WedAreaInfo>(std::move(wed));
            ctx.activeArea.store(area);
            ctx.wed.store(std::move(wedSnapshot));
            const auto cachedWed = ctx.wed.load();

            if (!same_resref(cachedWed->areaResrefView(), ctx.lastLoggedWedArea)) {
                LOG_INFO("Loaded WED {}: overlays={}, base={}x{}, liquidOverlayMask=0x{:02X}",
                         cachedWed->areaResrefView(), cachedWed->overlayCount, cachedWed->baseWidth,
                         cachedWed->baseHeight, liquidMask);
                // Per-overlay classification: an overlay the engine draws but we
                // did NOT classify as liquid shows vanilla water through base-tile
                // holes, unstyled (suspect for the v18/v19 teal squares).
                for (std::size_t i = 1; i < cachedWed->overlays.size() && i <= 7; ++i) {
                    const auto &overlay = cachedWed->overlays[i];
                    if (overlay.tilesetResrefView().empty()) {
                        continue;
                    }
                    LOG_INFO("  overlay {}: tileset={} mode={} cells={}", i, overlay.tilesetResrefView(),
                             game::tile_liquid_mode_name(overlay.liquidMode), overlay.coverageCells);
                }
                ctx.lastLoggedWedArea = cachedWed->areaResref;
            } else {
                LOG_DEBUG("Reloaded WED {}: overlays={}, base={}x{}, liquidOverlayMask=0x{:02X}",
                          cachedWed->areaResrefView(), cachedWed->overlayCount, cachedWed->baseWidth,
                          cachedWed->baseHeight, liquidMask);
            }

            // Prepare CPU data only. The Seam-tone render-thread hook owns every GL
            // upload, so LoadArea never depends on a current context.
            std::unordered_map<std::uint32_t, std::optional<game::TileAlpha>> tileAlphaCache;
            double tintSum[3] = {0.0, 0.0, 0.0};
            std::size_t tintTiles = 0;
            const auto &tileSets = areaSnapshot.m_cInfinity.pTileSets;
            const auto tileAlpha = [&](std::size_t overlayIndex,
                                       std::uint16_t tileIndex) -> std::optional<game::TileAlpha> {
                const auto key = static_cast<std::uint32_t>(overlayIndex) << 16 | tileIndex;
                if (const auto cached = tileAlphaCache.find(key); cached != tileAlphaCache.end()) {
                    return cached->second;
                }
                auto &slot = tileAlphaCache[key];
                if (overlayIndex >= tileSets.size() || !tileSets[overlayIndex]) return slot;

                game::CInfTileSet tileSet{};
                if (!core::safe_read(tileSets[overlayIndex], tileSet) || !tileSet.pResTiles ||
                    tileIndex >= tileSet.nTiles)
                    return slot;

                void *wrapper = nullptr;
                void *tileResPtr = nullptr;
                if (!core::safe_read(tileSet.pResTiles + tileIndex, wrapper) || !wrapper ||
                    !core::safe_read(wrapper, tileResPtr) || !tileResPtr)
                    return slot;

                game::CRes tileRes{};
                if (!core::safe_read(tileResPtr, tileRes) || !tileRes.bLoaded || !tileRes.pData ||
                    tileRes.nSize < game::kPaletteTileBytes ||
                    !core::is_readable(tileRes.pData, game::kPaletteTileBytes))
                    return slot;

                slot = game::decode_palette_tile_alpha(static_cast<const std::uint8_t *>(tileRes.pData), tileRes.nSize);
                if (slot && overlayIndex < cachedWed->overlays.size() &&
                    cachedWed->overlays[overlayIndex].liquidMode == game::TileLiquidMode::Water) {
                    if (const auto average = game::palette_tile_average_color(
                            static_cast<const std::uint8_t *>(tileRes.pData), tileRes.nSize)) {
                        tintSum[0] += (*average)[0];
                        tintSum[1] += (*average)[1];
                        tintSum[2] += (*average)[2];
                        ++tintTiles;
                    }
                }
                return slot;
            };

            auto packed = game::build_fine_liquid_mask(*cachedWed, tileAlpha);
            if (!packed) {
                LOG_WARN("Fine liquid mask build failed; keeping the queued no-liquid generation");
                return;
            }

            std::array<float, 3> waterTint{0.5f, 0.5f, 0.5f};
            if (tintTiles > 0) {
                const auto inverse = 1.0 / static_cast<double>(tintTiles);
                waterTint = {static_cast<float>(tintSum[0] * inverse), static_cast<float>(tintSum[1] * inverse),
                             static_cast<float>(tintSum[2] * inverse)};
                LOG_INFO("Area water tint: ({:.3f}, {:.3f}, {:.3f}) from {} water tiles", waterTint[0], waterTint[1],
                         waterTint[2], tintTiles);
            }

            LOG_INFO("Fine liquid mask prepared: {}x{} texels, {} unique overlay tiles decoded", packed->width,
                     packed->height, tileAlphaCache.size());
            queue_area_gpu_snapshot(std::move(*packed), waterTint);
        } catch (const std::exception &e) {
            LOG_ERROR("Area WED refresh failed: {}", e.what());
            reset_gpu_area_state();
        } catch (...) {
            LOG_ERROR("Area WED refresh failed with an unknown exception");
            reset_gpu_area_state();
        }
    }

    void reset_gpu_area_state() noexcept {
        try {
            game::AreaCellTexture noLiquid{.width = 1, .height = 1, .texels = {0}};
            queue_area_gpu_snapshot(std::move(noLiquid), {0.5f, 0.5f, 0.5f});
        } catch (...) {
            // The previous generation remains valid; the draw gate stays off
            // until another refresh can publish a complete generation.
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

            auto &gl = game::gl::get_gl_functions();
            const auto context = game::gl::current_context();
            if (!context || !gl.textureUploadAvailable) return false;

            if (context != g_areaTextureContext) {
                g_areaTextureContext = context;
                g_areaTexture = 0;
                g_uploadedAreaGeneration = 0;
            }
            if (g_areaTexture != 0 && g_uploadedAreaGeneration == snapshot->generation) return true;

            core::GlStateGuard guard({2});
            if (g_areaTexture == 0) gl.glGenTextures(1, &g_areaTexture);
            gl.glActiveTexture(game::gl::TEXTURE0 + 2);
            gl.glBindTexture(game::gl::TEXTURE_2D, g_areaTexture);
            gl.glPixelStorei(game::gl::UNPACK_ALIGNMENT, 1);
            gl.glTexImage2D(game::gl::TEXTURE_2D, 0, game::gl::R8, snapshot->texture.width, snapshot->texture.height, 0,
                            game::gl::RED, game::gl::UNSIGNED_BYTE, snapshot->texture.texels.data());
            gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MIN_FILTER, game::gl::NEAREST);
            gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MAG_FILTER, game::gl::NEAREST);
            gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_S, game::gl::CLAMP_TO_EDGE);
            gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_T, game::gl::CLAMP_TO_EDGE);
            if (!game::gl::check_error("area liquid texture upload")) return false;

            g_uploadedAreaGeneration = snapshot->generation;
            probe::set_area_water_tint(snapshot->waterTint[0], snapshot->waterTint[1], snapshot->waterTint[2]);
            probe::set_area_world_size(static_cast<float>(snapshot->texture.width) * 8.0f,
                                       static_cast<float>(snapshot->texture.height) * 8.0f);
            LOG_INFO("Area liquid mask uploaded: {}x{} texels (unit 2, tex {}, generation {})", snapshot->texture.width,
                     snapshot->texture.height, g_areaTexture, snapshot->generation);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool bind_area_texture() noexcept {
        if (!flush_pending_gpu_upload() || g_areaTexture == 0) return false;
        auto &gl = game::gl::get_gl_functions();
        if (!gl.glActiveTexture || !gl.glBindTexture) return false;
        int previousActiveTexture = -1;
        if (gl.glGetIntegerv) gl.glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &previousActiveTexture);
        gl.glActiveTexture(game::gl::TEXTURE0 + 2);
        gl.glBindTexture(game::gl::TEXTURE_2D, g_areaTexture);
        if (previousActiveTexture >= 0) gl.glActiveTexture(static_cast<unsigned>(previousActiveTexture));
        return true;
    }

    void release_gpu_area_resources() noexcept {
        try {
            auto &gl = game::gl::get_gl_functions();
            if (g_areaTexture && g_areaTextureContext && game::gl::current_context() == g_areaTextureContext &&
                gl.glDeleteTextures) {
                gl.glDeleteTextures(1, &g_areaTexture);
            }
            g_areaTexture = 0;
            g_areaTextureContext = nullptr;
            g_uploadedAreaGeneration = 0;
            std::lock_guard lock(g_areaGpuMutex);
            g_latestAreaGpu.reset();
        } catch (...) {
            // Explicit shutdown must not escape into EEex.
        }
    }
} // namespace iee::area
