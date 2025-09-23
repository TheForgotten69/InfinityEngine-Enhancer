#pragma once
#include <atomic>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <string>
#include "iee/core/config.h"
#include "iee/game/game_addrs.h"
#include "iee/game/renderer.h"

namespace iee {
    struct ShaderInfo {
        unsigned int programId;
        std::string name;
        int lastTone;
    };

    struct AppContext {
        core::EngineConfig cfg{};
        game::GameAddresses addrs{};
        game::DrawApi draw{};

        std::atomic<int> lastTexId{-1};
        std::atomic<int> areaScale{1};
        std::atomic<bool> scaleDetected{false};
        std::atomic<int> detectionCount{0};

        bool isRenderHookActive{false};

        std::unordered_set<int> configuredTextures;

        int lastTone{-1};
        void *lastTis{nullptr};

        std::vector<ShaderInfo> shaderHistory;
        std::mutex shaderMutex;

        void reset_area_state() {
            lastTone = -1;
            lastTis = nullptr;
            lastTexId.store(-1);
            areaScale.store(1);
            scaleDetected.store(false);
            detectionCount.store(0);
            // Note: Don't clear configuredTextures - UI textures should persist
        }

        void reset_all_state() {
            reset_area_state();
            configuredTextures.clear();
            shaderHistory.clear();
            isRenderHookActive = false;
        }
    };
}
