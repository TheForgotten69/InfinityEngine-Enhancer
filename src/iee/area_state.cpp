#include "area_state.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>

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
        out.scrollX = static_cast<float>(newX) -
                      static_cast<float>(viewPortNotZoomed.left) * worldW / logicalW;
        out.scrollY = static_cast<float>(newY) -
                      static_cast<float>(viewPortNotZoomed.top) * worldH / logicalH;

        // Debug: raw inputs, rate-limited — pairs with F10 snapshots so the
        // transform arithmetic can be checked exactly against screenshots.
        static std::uint32_t s_lastLogTick = 0;
        const std::uint32_t nowTick = GetTickCount();
        if (nowTick - s_lastLogTick > 5000) {
            s_lastLogTick = nowTick;
            LOG_INFO("ViewXform raw: nNew=({}, {}) rVP=({}, {}, {}, {}) rVPNZ=({}, {}, {}, {}) -> scroll=({}, {}) viewWorld={}x{}",
                     newX, newY,
                     viewPort.left, viewPort.top, viewPort.right, viewPort.bottom,
                     viewPortNotZoomed.left, viewPortNotZoomed.top,
                     viewPortNotZoomed.right, viewPortNotZoomed.bottom,
                     out.scrollX, out.scrollY, out.viewWorldW, out.viewWorldH);
        }
        return true;
    }

    void refresh_wed_cache(AppContext &ctx, void *infGame) {
        ctx.activeArea.store(nullptr);
        std::atomic_store(&ctx.wed, std::shared_ptr<const game::WedAreaInfo>{});

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
                (void) ctx.draw.CRes_Demand(areaSnapshot.m_pResWED);
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
            game::read_runtime_resref(areaSnapshot.m_resref.m_resRef.data(), areaResref);
            LOG_WARN("LoadArea: failed to parse loaded WED for area {}", game::resref_view(areaResref));
            return;
        }

        const auto liquidMask = game::liquid_overlay_mask(wed);
        auto wedSnapshot = std::make_shared<const game::WedAreaInfo>(std::move(wed));
        ctx.activeArea.store(area);
        std::atomic_store(&ctx.wed, std::move(wedSnapshot));
        const auto cachedWed = std::atomic_load(&ctx.wed);

        if (!same_resref(cachedWed->areaResrefView(), ctx.lastLoggedWedArea)) {
            LOG_INFO("Loaded WED {}: overlays={}, base={}x{}, liquidOverlayMask=0x{:02X}",
                     cachedWed->areaResrefView(),
                     cachedWed->overlayCount,
                     cachedWed->baseWidth,
                     cachedWed->baseHeight,
                     liquidMask);
            ctx.lastLoggedWedArea = cachedWed->areaResref;
        } else {
            LOG_DEBUG("Reloaded WED {}: overlays={}, base={}x{}, liquidOverlayMask=0x{:02X}",
                      cachedWed->areaResrefView(),
                      cachedWed->overlayCount,
                      cachedWed->baseWidth,
                      cachedWed->baseHeight,
                      liquidMask);
        }

        // Upload the fine liquid mask (8px/texel from the painted overlay-tile
        // silhouette) as an R8 texture on reserved unit 2.
        // CAVEAT: this runs on the LoadArea thread, which may not own the GL
        // context. If in-game logs show GL errors here, move the upload to a
        // pending-work flag consumed by the frame hook (render thread).
        {
            auto &gl = game::gl::get_gl_functions();
            if (gl.textureUploadAvailable) {
                core::GlStateGuard guard;
                static unsigned s_areaTexture = 0;
                if (s_areaTexture == 0) gl.glGenTextures(1, &s_areaTexture);
                gl.glActiveTexture(game::gl::TEXTURE0 + 2);
                gl.glBindTexture(game::gl::TEXTURE_2D, s_areaTexture);
                gl.glPixelStorei(game::gl::UNPACK_ALIGNMENT, 1);

                // Overlay tile alpha straight from the engine's loaded tilesets,
                // memoized per unique (overlay, tile). Any unreadable step
                // returns nullopt and the stamper keeps the full-cell fallback.
                std::unordered_map<std::uint32_t, std::optional<game::TileAlpha>> tileAlphaCache;
                const auto &tileSets = areaSnapshot.m_cInfinity.pTileSets;
                const auto tileAlpha = [&](std::size_t overlayIndex,
                                           std::uint16_t tileIndex) -> std::optional<game::TileAlpha> {
                    const auto key = static_cast<std::uint32_t>(overlayIndex) << 16 | tileIndex;
                    if (const auto cached = tileAlphaCache.find(key); cached != tileAlphaCache.end()) {
                        return cached->second;
                    }
                    auto &slot = tileAlphaCache[key];
                    if (overlayIndex >= tileSets.size() || !tileSets[overlayIndex]) {
                        return slot;
                    }
                    game::CInfTileSet tileSet{};
                    if (!core::safe_read(tileSets[overlayIndex], tileSet) ||
                        !tileSet.pResTiles || tileIndex >= tileSet.nTiles) {
                        return slot;
                    }
                    void *resPtr = nullptr;
                    if (!core::safe_read(tileSet.pResTiles + tileIndex, resPtr) || !resPtr) {
                        return slot;
                    }
                    game::CRes tileRes{};
                    if (!core::safe_read(resPtr, tileRes) || !tileRes.bLoaded || !tileRes.pData ||
                        tileRes.nSize < game::kPaletteTileBytes ||
                        !core::is_readable(tileRes.pData, game::kPaletteTileBytes)) {
                        return slot;
                    }
                    slot = game::decode_palette_tile_alpha(
                            static_cast<const std::uint8_t *>(tileRes.pData), tileRes.nSize);
                    return slot;
                };

                // On any failure the previous area's mask must not survive the
                // transition: fall back to a 1x1 zero texel (mode 0 everywhere).
                static const std::uint8_t kNoLiquid = 0;
                const auto packed = game::build_fine_liquid_mask(*cachedWed, tileAlpha);
                const void *texels = packed ? static_cast<const void *>(packed->texels.data())
                                            : static_cast<const void *>(&kNoLiquid);
                const int width = packed ? packed->width : 1;
                const int height = packed ? packed->height : 1;

                gl.glTexImage2D(game::gl::TEXTURE_2D, 0, game::gl::R8,
                                width, height, 0,
                                game::gl::RED, game::gl::UNSIGNED_BYTE, texels);
                gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MIN_FILTER, game::gl::NEAREST);
                gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MAG_FILTER, game::gl::NEAREST);
                gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_S, game::gl::CLAMP_TO_EDGE);
                gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_T, game::gl::CLAMP_TO_EDGE);
                if (packed && game::gl::check_error("area liquid texture upload")) {
                    LOG_INFO("Fine liquid mask uploaded: {}x{} texels, {} unique overlay tiles decoded (unit 2, tex {})",
                             width, height, tileAlphaCache.size(), s_areaTexture);
                    // 8 world px per fine-mask texel.
                    probe::set_area_world_size(static_cast<float>(width) * 8.0f,
                                               static_cast<float>(height) * 8.0f);
                } else if (!packed) {
                    LOG_WARN("Fine liquid mask build failed; uploaded 1x1 no-liquid mask");
                } else {
                    LOG_WARN("Area liquid texture upload reported a GL error (likely wrong thread; see caveat)");
                }
            }
        }
    }
}
