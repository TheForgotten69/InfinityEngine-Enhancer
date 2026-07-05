#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "iee/core/config.h"
#include "iee/game/build_manifest.h"
#include "iee/game/game_addrs.h"
#include "iee/game/renderer.h"
#include "iee/game/resref_runtime.h"
#include "iee/game/wed_runtime.h"

namespace iee {
    // Shared infrastructure only. Per-feature state lives in the owning feature
    // module (e.g. features::tile_render_state()).
    struct AppContext {
        core::EngineConfig cfg{};
        const game::BuildManifest *manifest{};
        game::GameAddresses addrs{};
        game::DrawApi draw{};

        std::atomic<const game::CGameArea *> activeArea{nullptr};
        // CInfGame from the LoadArea hook; lets the render thread re-resolve
        // the active area when transitions settle after LoadArea returns.
        std::atomic<void *> infGame{nullptr};
        std::shared_ptr<const game::WedAreaInfo> wed{};
        game::ResrefBuffer lastLoggedWedArea{};

        bool isRenderHookActive{false};

        void reset_area_state() {
            activeArea.store(nullptr);
            std::atomic_store(&wed, std::shared_ptr<const game::WedAreaInfo>{});
            lastLoggedWedArea.fill('\0');
        }

        void reset_all_state() {
            reset_area_state();
            isRenderHookActive = false;
        }
    };
}
