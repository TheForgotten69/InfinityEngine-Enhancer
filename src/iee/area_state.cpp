#include "area_state.h"

#include <array>
#include <memory>
#include <string_view>

#include "app_context.h"
#include "iee/core/gl_state_guard.h"
#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"
#include "iee/game/area_texture.h"
#include "iee/game/opengl_types.h"
#include "iee/game/resref_runtime.h"
#include "iee/game/tile_liquid.h"
#include "iee/game/wed_runtime.h"

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

        const auto *base = reinterpret_cast<const std::byte *>(area);
        const auto *offsetXAddr = base + offsetof(game::CGameArea, m_cInfinity) + offsetof(game::CInfinity, nOffsetX);
        const auto *offsetYAddr = base + offsetof(game::CGameArea, m_cInfinity) + offsetof(game::CInfinity, nOffsetY);
        return core::safe_read(offsetXAddr, outOffsetX) && core::safe_read(offsetYAddr, outOffsetY);
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

        // Upload the per-cell overlay flags as an R8 texture on reserved unit 2.
        // CAVEAT: this runs on the LoadArea thread, which may not own the GL
        // context. If in-game logs show GL errors here, move the upload to a
        // pending-work flag consumed by the frame hook (render thread).
        if (const auto packed = game::pack_area_cell_texture(*cachedWed)) {
            auto &gl = game::gl::get_gl_functions();
            if (gl.textureUploadAvailable) {
                core::GlStateGuard guard;
                static unsigned s_areaTexture = 0;
                if (s_areaTexture == 0) gl.glGenTextures(1, &s_areaTexture);
                gl.glActiveTexture(game::gl::TEXTURE0 + 2);
                gl.glBindTexture(game::gl::TEXTURE_2D, s_areaTexture);
                gl.glPixelStorei(game::gl::UNPACK_ALIGNMENT, 1);
                gl.glTexImage2D(game::gl::TEXTURE_2D, 0, game::gl::R8,
                                packed->width, packed->height, 0,
                                game::gl::RED, game::gl::UNSIGNED_BYTE, packed->texels.data());
                gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MIN_FILTER, game::gl::NEAREST);
                gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MAG_FILTER, game::gl::NEAREST);
                if (game::gl::check_error("area cell texture upload")) {
                    LOG_INFO("Area cell texture uploaded: {}x{} (unit 2, tex {})",
                             packed->width, packed->height, s_areaTexture);
                } else {
                    LOG_WARN("Area cell texture upload reported a GL error (likely wrong thread; see caveat)");
                }
            }
        }
    }
}
