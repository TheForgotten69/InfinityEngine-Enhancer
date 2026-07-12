#include "game_addrs.h"
#include "build_manifest.h"
#include "iee/core/pattern_scanner.h"
#include "iee/core/logger.h"
#include "iee/core/config.h"

namespace iee::game {
    bool resolve_addresses(GameAddresses &out, const core::EngineConfig &cfg, const BuildManifest &manifest) {
        (void) cfg;
        out = {};
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
        std::size_t loadAreaMatches = 0;
        out.LoadArea = reinterpret_cast<std::uintptr_t>(
            core::find_unique_in_module(nullptr, manifest.patterns.loadArea, &loadAreaMatches));

        LOG_DEBUG("Searching for RenderTexture pattern...");
        std::size_t renderTextureMatches = 0;
        out.RenderTexture = reinterpret_cast<std::uintptr_t>(
            core::find_unique_in_module(nullptr, manifest.patterns.renderTexture, &renderTextureMatches));

        const bool success = out.LoadArea && out.RenderTexture;

        if (success) {
            LOG_DEBUG("Pattern scanning SUCCESS:");
            const auto foundLoadAreaRVA = out.LoadArea - moduleBase;
            const auto foundRenderTextureRVA = out.RenderTexture - moduleBase;

            LOG_DEBUG("  LoadArea: 0x{:X} (RVA: 0x{:X})", out.LoadArea, foundLoadAreaRVA);
            LOG_DEBUG("  RenderTexture: 0x{:X} (RVA: 0x{:X})", out.RenderTexture, foundRenderTextureRVA);

            if (foundLoadAreaRVA == manifest.referenceRvas.loadArea) {
                LOG_DEBUG("  LoadArea RVA matches manifest reference 0x{:X}", manifest.referenceRvas.loadArea);
            } else {
                LOG_WARN("  LoadArea RVA differs from manifest reference 0x{:X}", manifest.referenceRvas.loadArea);
            }

            if (foundRenderTextureRVA == manifest.referenceRvas.renderTexture) {
                LOG_DEBUG("  RenderTexture RVA matches manifest reference 0x{:X}", manifest.referenceRvas.renderTexture);
            } else {
                LOG_WARN("  RenderTexture RVA differs from manifest reference 0x{:X}",
                         manifest.referenceRvas.renderTexture);
            }

            out.initialized = true;
        } else {
            LOG_ERROR("Build signature validation failed; refusing unsafe reference RVAs");
            LOG_ERROR("  LoadArea matches: {}", loadAreaMatches);
            LOG_ERROR("  RenderTexture matches: {}", renderTextureMatches);
            out = {};
            return false;
        }

        return out.initialized;
    }
}
