#include "game_addrs.h"
#include "build_manifest.h"
#include "iee/core/pattern_scanner.h"
#include "iee/core/logger.h"
#include "iee/core/config.h"

namespace iee::game {
    bool resolve_addresses(GameAddresses &out, const core::EngineConfig &cfg, const BuildManifest &manifest) {
        auto moduleInfo = core::get_module_span(nullptr);
        if (!moduleInfo || !moduleInfo->base || !moduleInfo->size) {
            LOG_ERROR("Failed to get module information");
            return false;
        }

        const auto moduleBase = reinterpret_cast<std::uintptr_t>(moduleInfo->base);
        LOG_DEBUG("Scanning module: Base=0x{:X}, Size=0x{:X}", moduleBase, moduleInfo->size);
        LOG_INFO("Using build manifest: {}", manifest.buildId);

        LOG_DEBUG("Attempting pattern scanning with build manifest patterns...");

        LOG_DEBUG("Searching for LoadArea pattern...");
        out.LoadArea = reinterpret_cast<std::uintptr_t>(
            core::find_first_in_module(nullptr, manifest.patterns.loadArea)
        );

        LOG_DEBUG("Searching for RenderTexture pattern...");
        out.RenderTexture = reinterpret_cast<std::uintptr_t>(
            core::find_first_in_module(nullptr, manifest.patterns.renderTexture)
        );

        const bool success = out.LoadArea && out.RenderTexture;

        if (success) {
            LOG_DEBUG("Pattern scanning SUCCESS:");
            const auto foundLoadAreaRVA = out.LoadArea - moduleBase;
            const auto foundRenderTextureRVA = out.RenderTexture - moduleBase;

            LOG_DEBUG("  LoadArea: 0x{:X} (RVA: 0x{:X})", out.LoadArea, foundLoadAreaRVA);
            LOG_DEBUG("  RenderTexture: 0x{:X} (RVA: 0x{:X})", out.RenderTexture, foundRenderTextureRVA);

            if (foundLoadAreaRVA == manifest.fallbacks.loadArea) {
                LOG_DEBUG("  LoadArea RVA matches manifest fallback 0x{:X}", manifest.fallbacks.loadArea);
            } else {
                LOG_WARN("  LoadArea RVA differs from manifest fallback 0x{:X}", manifest.fallbacks.loadArea);
            }

            if (foundRenderTextureRVA == manifest.fallbacks.renderTexture) {
                LOG_DEBUG("  RenderTexture RVA matches manifest fallback 0x{:X}", manifest.fallbacks.renderTexture);
            } else {
                LOG_WARN("  RenderTexture RVA differs from manifest fallback 0x{:X}",
                         manifest.fallbacks.renderTexture);
            }

            out.initialized = true;
        } else {
            LOG_WARN("Pattern scanning failed - using configured or manifest fallback RVAs");
            if (!out.LoadArea) {
                LOG_DEBUG("  LoadArea pattern not found");
            }
            if (!out.RenderTexture) {
                LOG_DEBUG("  RenderTexture pattern not found");
            }
        }

        if (!success) {
            const auto fallbackLoadAreaRva = cfg.cachedLoadAreaRVA != 0
                                                 ? cfg.cachedLoadAreaRVA
                                                 : manifest.fallbacks.loadArea;
            const auto fallbackRenderTextureRva = cfg.cachedRenderTextureRVA != 0
                                                      ? cfg.cachedRenderTextureRVA
                                                      : manifest.fallbacks.renderTexture;

            out.LoadArea = moduleBase + fallbackLoadAreaRva;
            out.RenderTexture = moduleBase + fallbackRenderTextureRva;
            out.initialized = out.LoadArea != 0 && out.RenderTexture != 0;

            LOG_DEBUG("Fallback addresses:");
            LOG_DEBUG("  LoadArea: 0x{:X} (RVA: 0x{:X})", out.LoadArea, fallbackLoadAreaRva);
            LOG_DEBUG("  RenderTexture: 0x{:X} (RVA: 0x{:X})", out.RenderTexture, fallbackRenderTextureRva);
        }

        return out.initialized;
    }
}
