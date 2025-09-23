#include "game_addrs.h"
#include "iee/core/pattern_scanner.h"
#include "iee/core/logger.h"
#include "iee/core/config.h"
#include <unordered_map>
#include <optional>

namespace iee::game {
    // Known RVA fallbacks per build
    struct FallbackRVAs {
        std::uintptr_t LoadArea;
        std::uintptr_t RenderTexture;
    };

    static const std::unordered_map<std::string, FallbackRVAs> kKnownBuilds = {
        {"BGEE 2.6.6.x", {0x27E710, 0x4247E0}},
        // TODO: Add more builds if necessary
    };

    std::optional<std::string> detect_build() {
        return "BGEE 2.6.6.x";
    }

    static std::optional<FallbackRVAs> get_fallback_rvas(const std::string &buildId) {
        auto it = kKnownBuilds.find(buildId);
        if (it != kKnownBuilds.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool resolve_addresses(GameAddresses &out, const core::EngineConfig &cfg) {
        auto moduleInfo = core::get_module_span(nullptr);
        if (!moduleInfo || !moduleInfo->base || !moduleInfo->size) {
            LOG_ERROR("Failed to get module information");
            return false;
        }

        uintptr_t moduleBase = (uintptr_t) moduleInfo->base;
        LOG_DEBUG("Scanning module: Base=0x{:X}, Size=0x{:X}", moduleBase, moduleInfo->size);

        bool success = false;

        LOG_DEBUG("Attempting pattern scanning with enhanced patterns...");

        LOG_DEBUG("Searching for LoadArea pattern...");
        void *loadAreaAddr = core::find_first_in_module(nullptr, PatternSignatures::LOAD_AREA);
        out.LoadArea = (uintptr_t) loadAreaAddr;

        LOG_DEBUG("Searching for RenderTexture pattern...");
        void *renderTextureAddr = core::find_first_in_module(nullptr, PatternSignatures::RENDER_TEXTURE);
        out.RenderTexture = (uintptr_t) renderTextureAddr;

        success = out.LoadArea && out.RenderTexture;

        if (success) {
            LOG_DEBUG("Pattern scanning SUCCESS:");
            uintptr_t foundLoadAreaRVA = out.LoadArea - moduleBase;
            uintptr_t foundRenderTextureRVA = out.RenderTexture - moduleBase;

            LOG_DEBUG("  LoadArea: 0x{:X} (RVA: 0x{:X})", out.LoadArea, foundLoadAreaRVA);
            LOG_DEBUG("  RenderTexture: 0x{:X} (RVA: 0x{:X})", out.RenderTexture, foundRenderTextureRVA);

            // Verify against expected RVAs if we have them
            if (auto buildId = detect_build()) {
                if (auto fallbacks = get_fallback_rvas(*buildId)) {
                    if (foundLoadAreaRVA == fallbacks->LoadArea) {
                        LOG_DEBUG("  ✓ LoadArea RVA matches expected 0x{:X}", fallbacks->LoadArea);
                    } else {
                        LOG_WARN("  ⚠ LoadArea RVA differs from expected 0x{:X}", fallbacks->LoadArea);
                    }

                    if (foundRenderTextureRVA == fallbacks->RenderTexture) {
                        LOG_DEBUG("  ✓ RenderTexture RVA matches expected 0x{:X}", fallbacks->RenderTexture);
                    } else {
                        LOG_WARN("  ⚠ RenderTexture RVA differs from expected 0x{:X}", fallbacks->RenderTexture);
                    }
                }
            }

            out.initialized = true;
        } else {
            LOG_WARN("Pattern scanning failed - using cached addresses");
            if (!out.LoadArea)
                LOG_DEBUG("  LoadArea pattern not found");
            if (!out.RenderTexture)
                LOG_DEBUG("  RenderTexture pattern not found");
        }

        if (!success) {
            LOG_DEBUG("Using cached addresses from config");

            out.LoadArea = moduleBase + cfg.cachedLoadAreaRVA;
            out.RenderTexture = moduleBase + cfg.cachedRenderTextureRVA;
            out.initialized = true;

            LOG_DEBUG("Cached addresses:");
            LOG_DEBUG("  LoadArea: 0x{:X} (RVA: 0x{:X})", out.LoadArea, cfg.cachedLoadAreaRVA);
            LOG_DEBUG("  RenderTexture: 0x{:X} (RVA: 0x{:X})", out.RenderTexture, cfg.cachedRenderTextureRVA);
        }

        return out.initialized;
    }
}
