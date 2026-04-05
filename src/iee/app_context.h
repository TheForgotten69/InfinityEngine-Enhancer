#pragma once
#include <atomic>

#include "iee/core/config.h"
#include "iee/game/build_manifest.h"
#include "iee/game/game_addrs.h"
#include "iee/game/renderer.h"

namespace iee {
    struct AppContext {
        core::EngineConfig cfg{};
        const game::BuildManifest *manifest{};
        game::GameAddresses addrs{};
        game::DrawApi draw{};

        std::atomic<int> lastTexId{-1};
        std::atomic<int> areaScale{1};
        std::atomic<bool> scaleDetected{false};
        std::atomic<int> detectionCount{0};

        bool isRenderHookActive{false};

        void reset_area_state() {
            lastTexId.store(-1);
            areaScale.store(1);
            scaleDetected.store(false);
            detectionCount.store(0);
        }

        void reset_all_state() {
            reset_area_state();
            isRenderHookActive = false;
        }
    };
}
