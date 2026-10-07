#include "iee/game/draw_queue.h"
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

        // Detour-tolerant recovery: EEex (which loads this DLL) installs its own
        // engine hooks, and on some builds it detours a target's prologue before
        // our scan runs, so the signature no longer appears in memory. The build
        // identity gate has already positively confirmed this exact executable
        // and the reference RVA was offline-validated for it, so re-confirm the
        // target at that RVA while ignoring a detour-sized prologue. This never
        // fabricates an address on an unknown build: identity must match first,
        // and the pattern tail past the prologue must still match the original.
        const auto recover = [&](const char *name, std::uintptr_t &target,
                                 std::size_t matches, std::uintptr_t referenceRva,
                                 std::string_view pattern) {
            if (target || matches != 0) return;
            if (auto *address =
                    core::confirm_pattern_with_patched_prologue(nullptr, referenceRva, pattern)) {
                target = reinterpret_cast<std::uintptr_t>(address);
                LOG_WARN("{} prologue is detoured (likely EEex); recovered at reference RVA 0x{:X} "
                         "by verifying the un-patched pattern tail",
                         name, referenceRva);
            }
        };
        recover("LoadArea", out.LoadArea, loadAreaMatches, manifest.referenceRvas.loadArea,
                manifest.patterns.loadArea);
        recover("RenderTexture", out.RenderTexture, renderTextureMatches,
                manifest.referenceRvas.renderTexture, manifest.patterns.renderTexture);

        // Optional target: point-effect BAM replacement. Failure only keeps
        // the engine's authored draws; never blocks initialization.
        if (!manifest.patterns.staticRender.empty()) {
            std::size_t staticRenderMatches = 0;
            out.StaticRender = reinterpret_cast<std::uintptr_t>(core::find_unique_in_module(
                nullptr, manifest.patterns.staticRender, &staticRenderMatches));
            recover("StaticRender", out.StaticRender, staticRenderMatches,
                    manifest.referenceRvas.staticRender, manifest.patterns.staticRender);
            if (out.StaticRender) {
                LOG_INFO("CGameStatic::Render resolved at RVA 0x{:X} (reference 0x{:X})",
                         out.StaticRender - moduleBase, manifest.referenceRvas.staticRender);
            } else {
                LOG_WARN("CGameStatic::Render pattern matched {} times; authored fire/smoke "
                         "draws will not be replaced",
                         staticRenderMatches);
            }
        }

        const auto resolveOptional = [&](const char *name, std::string_view pattern,
                                         std::uintptr_t referenceRva) -> std::uintptr_t {
            if (pattern.empty()) return 0;
            std::size_t matches = 0;
            auto target = reinterpret_cast<std::uintptr_t>(
                core::find_unique_in_module(nullptr, pattern, &matches));
            recover(name, target, matches, referenceRva, pattern);
            if (!target) LOG_WARN("{} pattern matched {} times", name, matches);
            return target;
        };

        // Optional targets: sprite movement smoothing. Resolved only when
        // requested, and only as a complete set.
        if (cfg.smoothSpriteMovement || cfg.interpolateAnimations) {
            const auto &patterns = manifest.patterns;
            const auto &rvas = manifest.referenceRvas;
            out.SpriteRender =
                resolveOptional("CGameSprite::Render", patterns.spriteRender, rvas.spriteRender);
            out.SpriteRenderMarkers = resolveOptional(
                "CGameSprite::RenderMarkers", patterns.spriteRenderMarkers, rvas.spriteRenderMarkers);
            out.SpriteRenderHealthBar =
                resolveOptional("CGameSprite::RenderHealthBar", patterns.spriteRenderHealthBar,
                                rvas.spriteRenderHealthBar);
            if (cfg.interpolateAnimations) {
                // The twin accessors cannot be told apart by signature, so each
                // is confirmed in full at its reference RVA on this
                // identity-checked build.
                const auto confirmTwin = [&](std::uintptr_t rva) -> std::uintptr_t {
                    if (!rva || patterns.vidCellFrameAccessor.empty()) return 0;
                    return reinterpret_cast<std::uintptr_t>(
                        core::confirm_pattern_with_patched_prologue(
                            nullptr, rva, patterns.vidCellFrameAccessor, 0));
                };
                out.VidCellGetFrame = resolveOptional("CVidCell::GetFrame",
                                                      patterns.vidCellGetFrame,
                                                      rvas.vidCellGetFrame);
                out.VidCellGetCurrentCenterPoint = confirmTwin(rvas.vidCellGetCurrentCenterPoint);
                out.VidCellGetCurrentFrameSize = confirmTwin(rvas.vidCellGetCurrentFrameSize);
                if (!(out.VidCellGetFrame && out.VidCellGetCurrentCenterPoint &&
                      out.VidCellGetCurrentFrameSize)) {
                    out.VidCellGetFrame = out.VidCellGetCurrentCenterPoint =
                        out.VidCellGetCurrentFrameSize = 0;
                    LOG_WARN("Animation interpolation disabled: this build has no complete set "
                             "of CVidCell frame targets");
                }
            }
            for (std::size_t index = 0;
                 cfg.smoothSpriteMovement && index < manifest.smoothedObjectRenders.size();
                 ++index) {
                const auto &target = manifest.smoothedObjectRenders[index];
                if (!target.name) continue;
                out.SmoothedObjectRenders[index] =
                    resolveOptional(target.name, target.pattern, target.referenceRva);
            }
            if (cfg.smoothSpriteMovement) {
                out.ParticleUpdate = resolveOptional("CParticle::AsynchronousUpdate",
                                                     patterns.particleUpdate, rvas.particleUpdate);
                out.ParticleRender = resolveOptional("CParticle::Render", patterns.particleRender,
                                                     rvas.particleRender);
                if (!(out.ParticleUpdate && out.ParticleRender)) {
                    out.ParticleUpdate = out.ParticleRender = 0;
                }
            }
            if (out.SpriteRender && out.SpriteRenderMarkers && out.SpriteRenderHealthBar) {
                LOG_INFO("Sprite render targets resolved at RVA 0x{:X} / 0x{:X} / 0x{:X}",
                         out.SpriteRender - moduleBase, out.SpriteRenderMarkers - moduleBase,
                         out.SpriteRenderHealthBar - moduleBase);
            } else {
                out.SpriteRender = out.SpriteRenderMarkers = out.SpriteRenderHealthBar = 0;
                LOG_WARN("Sprite movement smoothing disabled: this build has no complete set of "
                         "sprite render targets");
            }
        }

        // Optional targets: world post passes. Resolved only when requested
        // (or when debug hotkeys can switch them on), and only as a pair.
        if (cfg.softFogOfWar || cfg.bloom || cfg.spriteUpscale || cfg.enableDebugHotkeys) {
            out.RenderFog = resolveOptional("CInfinity::RenderFog", manifest.patterns.renderFog,
                                            manifest.referenceRvas.renderFog);
            out.DrawFlush = resolveOptional("DrawFlush_GL", manifest.patterns.drawFlush,
                                            manifest.referenceRvas.drawFlush);
            if (out.RenderFog && out.DrawFlush) {
                LOG_INFO("World post targets resolved at RVA 0x{:X} / 0x{:X}",
                         out.RenderFog - moduleBase, out.DrawFlush - moduleBase);
                out.DrawQueueCount = draw_queue_count_address(manifest.drawQueue, out.DrawFlush);
                out.DrawQueueCommands =
                    draw_queue_commands_address(manifest.drawQueue, out.DrawQueueCount);
                if (out.DrawQueueCount && out.DrawQueueCommands) {
                    LOG_INFO("Draw queue resolved: count at RVA 0x{:X}, commands at RVA 0x{:X}",
                             out.DrawQueueCount - moduleBase, out.DrawQueueCommands - moduleBase);
                    if (manifest.spriteAtlas.valid() &&
                        (cfg.spriteUpscale || cfg.enableDebugHotkeys)) {
                        out.TextureUpload =
                            resolveOptional("TexSubImage_GL", manifest.patterns.textureUpload,
                                            manifest.referenceRvas.textureUpload);
                        if (!out.TextureUpload && cfg.spriteUpscale) {
                            LOG_WARN("Sprite upscaling disabled: TexSubImage_GL did not resolve");
                        }
                    }
                } else {
                    out.DrawQueueCount = out.DrawQueueCommands = 0;
                    if (cfg.bloom) {
                        LOG_WARN("Bloom disabled: the draw queue was not found in DrawFlush_GL");
                    }
                }
            } else {
                const bool known =
                    !manifest.patterns.renderFog.empty() && !manifest.patterns.drawFlush.empty();
                out.RenderFog = out.DrawFlush = 0;
                if (cfg.softFogOfWar || cfg.bloom || cfg.spriteUpscale) {
                    LOG_WARN("Soft fog of war, bloom and sprite upscaling disabled: {}",
                             known ? "RenderFog/DrawFlush did not resolve uniquely"
                                   : "this build has no RenderFog/DrawFlush targets");
                }
            }
        }

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
