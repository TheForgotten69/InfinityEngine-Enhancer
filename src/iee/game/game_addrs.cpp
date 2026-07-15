#include "game_addrs.h"

#include <array>
#include <iterator>
#include <string>

#include <spdlog/fmt/fmt.h>

#include "build_manifest.h"
#include "iee/core/config.h"
#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"

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
            LOG_ERROR("  LoadArea matches: {} (found RVA 0x{:X}, reference 0x{:X})", loadAreaMatches,
                      out.LoadArea ? out.LoadArea - moduleBase : 0, manifest.referenceRvas.loadArea);
            LOG_ERROR("  RenderTexture matches: {} (reference 0x{:X})", renderTextureMatches,
                      manifest.referenceRvas.renderTexture);

            // Diagnosis aid: a signature that exists on disk but not in memory
            // means another component (loader, EEex, overlay) patched the
            // prologue before our scan. Dump the live bytes at each reference
            // RVA so the divergence is visible in one failing run.
            const auto dumpReference = [&](const char *name, std::uintptr_t rva) {
                const auto *address = reinterpret_cast<const std::uint8_t *>(moduleBase + rva);
                std::array<std::uint8_t, 24> bytes{};
                if (!core::safe_read(address, bytes)) {
                    LOG_ERROR("  {} bytes at reference RVA 0x{:X}: <unreadable>", name, rva);
                    return;
                }
                std::string hex;
                hex.reserve(bytes.size() * 3);
                for (const auto value : bytes) {
                    fmt::format_to(std::back_inserter(hex), "{:02X} ", value);
                }
                LOG_ERROR("  {} bytes at reference RVA 0x{:X}: {}", name, rva, hex);
            };
            dumpReference("LoadArea", manifest.referenceRvas.loadArea);
            dumpReference("RenderTexture", manifest.referenceRvas.renderTexture);
            out = {};
            return false;
        }

        return out.initialized;
    }
}
