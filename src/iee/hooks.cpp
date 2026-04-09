#include "hooks.h"
#include "app_context.h"
#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/game/game_types.h"
#include "iee/game/renderer.h"
#include "iee/game/tile_upscale.h"
#include "iee/game/tis_runtime.h"
#include "iee/core/pattern_scanner.h"
#include <array>
#include <optional>
#include <windows.h>

namespace iee::hooks {
    // region Hook function type definitions
    using Fn_LoadArea = void* (*)(void *, void *, unsigned char, unsigned char, unsigned char);
    using Fn_RenderTexture = void (*)(void *, int, void *, int, int, unsigned long);
    using Fn_DrawColorTone = void (*)(int);

    // Hook management - initialize MinHook
    static core::HookInit hkInit;  // Required for MinHook initialization
    static core::Hook<Fn_LoadArea> H_LoadArea;
    static core::Hook<Fn_RenderTexture> H_RenderTexture;
    static core::Hook<Fn_DrawColorTone> H_DrawColorTone;

    static AppContext *g_ctx = nullptr;

    static std::atomic<uint32_t> g_currentAreaId{0};

    using game::DrawMode;
    using game::ShaderTone;

    static void log_pointer_dwords(const char *label, const void *address) {
        const auto value = reinterpret_cast<std::uintptr_t>(address);
        if (!address) {
            LOG_WARN("  {} = null", label);
            return;
        }

        std::array<std::uint32_t, 6> words{};
        if (!core::safe_read(address, words)) {
            LOG_WARN("  {} @ 0x{:X}: unreadable", label, value);
            return;
        }

        LOG_WARN("  {} @ 0x{:X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 label,
                 value,
                 words[0],
                 words[1],
                 words[2],
                 words[3],
                 words[4],
                 words[5]);
    }

    static void log_tis_header_diagnostics(void *vidTile,
                                           const game::TileInfo &tileInfo,
                                           const char *reason,
                                           std::optional<std::uint32_t> detectedTileDimension,
                                           bool includeCandidatePointers) {
        game::CResTileSet tilesetSnapshot{};
        const bool haveTilesetSnapshot = tileInfo.tileset && core::safe_read(tileInfo.tileset, tilesetSnapshot);

        const auto dataPointer = haveTilesetSnapshot ? tilesetSnapshot.baseclass_0.pData : nullptr;
        const auto dataSize = haveTilesetSnapshot ? tilesetSnapshot.baseclass_0.nSize : 0U;
        const auto dataCount = haveTilesetSnapshot ? tilesetSnapshot.baseclass_0.nCount : 0U;
        const auto page = (tileInfo.table && tileInfo.index >= 0) ? tileInfo.table[tileInfo.index].page : -1;
        const auto u = (tileInfo.table && tileInfo.index >= 0) ? tileInfo.table[tileInfo.index].u : -1;
        const auto v = (tileInfo.table && tileInfo.index >= 0) ? tileInfo.table[tileInfo.index].v : -1;

        if (detectedTileDimension) {
            LOG_WARN("{}: vidTile=0x{:X} resource=0x{:X} tileset=0x{:X} header=0x{:X} pData=0x{:X} nSize=0x{:X} nCount=0x{:X} runtimeTileDimension=0x{:X} tileIndex={} page={} uv=({}, {}) detectedTileDimension=0x{:X}",
                     reason,
                     reinterpret_cast<std::uintptr_t>(vidTile),
                     reinterpret_cast<std::uintptr_t>(tileInfo.resource),
                     reinterpret_cast<std::uintptr_t>(tileInfo.tileset),
                     reinterpret_cast<std::uintptr_t>(tileInfo.header),
                     reinterpret_cast<std::uintptr_t>(dataPointer),
                     dataSize,
                     dataCount,
                     tileInfo.runtimeTileDimension,
                     tileInfo.index,
                     page,
                     u,
                     v,
                     *detectedTileDimension);
        } else {
            LOG_WARN("{}: vidTile=0x{:X} resource=0x{:X} tileset=0x{:X} header=0x{:X} pData=0x{:X} nSize=0x{:X} nCount=0x{:X} runtimeTileDimension=0x{:X} tileIndex={} page={} uv=({}, {})",
                     reason,
                     reinterpret_cast<std::uintptr_t>(vidTile),
                     reinterpret_cast<std::uintptr_t>(tileInfo.resource),
                     reinterpret_cast<std::uintptr_t>(tileInfo.tileset),
                     reinterpret_cast<std::uintptr_t>(tileInfo.header),
                     reinterpret_cast<std::uintptr_t>(dataPointer),
                     dataSize,
                     dataCount,
                     tileInfo.runtimeTileDimension,
                     tileInfo.index,
                     page,
                     u,
                     v);
        }

        log_pointer_dwords("header", tileInfo.header);
        log_pointer_dwords("pData", dataPointer);

        if (!includeCandidatePointers || !tileInfo.tileset) {
            return;
        }

        const auto *tilesetBytes = reinterpret_cast<const std::byte *>(tileInfo.tileset);
        constexpr std::size_t candidateSlots = 4;
        const auto baseOffset = sizeof(game::CRes);

        for (std::size_t slot = 0; slot < candidateSlots; ++slot) {
            const auto offset = baseOffset + slot * sizeof(void *);
            std::uintptr_t candidate = 0;
            if (!core::safe_read(tilesetBytes + offset, candidate)) {
                LOG_WARN("  tileset[+0x{:X}] unreadable", offset);
                continue;
            }

            LOG_WARN("  tileset[+0x{:X}] = 0x{:X}", offset, candidate);

            const auto *candidatePtr = reinterpret_cast<const void *>(candidate);
            if (!candidatePtr || candidatePtr == tileInfo.header || candidatePtr == dataPointer) {
                continue;
            }

            LOG_WARN("  candidate(+0x{:X}) dump follows", offset);
            log_pointer_dwords("candidate", candidatePtr);
        }
    }

    // Function to trigger cache reset from LoadArea
    static void reset_thread_local_cache() {
        g_currentAreaId.fetch_add(1); // Increment to invalidate all thread-local caches
    }

    // LoadArea hook - reset area-specific state for new area detection
    static void *Detour_LoadArea(void *thisPtr, void *pAreaNameString, unsigned char a2, unsigned char a3,
                                 unsigned char a4) {
        if (!g_ctx) {
            return H_LoadArea.original()(thisPtr, pAreaNameString, a2, a3, a4);
        }

        LOG_DEBUG("LoadArea called - resetting scale detection for new area");

        auto &ctx = *g_ctx;
        ctx.reset_area_state();

        // Reset thread-local cache for new area
        reset_thread_local_cache();

        // Re-enable RenderTexture hook for new area detection
        if (!ctx.isRenderHookActive) {
            try {
                H_RenderTexture.enable();
                ctx.isRenderHookActive = true;
                LOG_DEBUG("Re-enabled RenderTexture hook for new area detection");
            } catch (const std::exception &e) {
                LOG_WARN("Failed to re-enable RenderTexture hook: {}", e.what());
            }
        }

        return H_LoadArea.original()(thisPtr, pAreaNameString, a2, a3, a4);
    }

    // DrawColorTone hook - simple passthrough with tracking
    static void Detour_DrawColorTone(int mode) {
        H_DrawColorTone.original()(mode);
    }

    // Main RenderTexture hook - handles HD detection and texture enhancement
    static void Detour_RenderTexture(void *thisPtr, int texId, void *unused, int x, int y, unsigned long flags) {
        // Safety check - ensure context is valid
        if (!g_ctx || !g_ctx->manifest) {
            return; // Context destroyed, don't process
        }

        auto &ctx = *g_ctx;

        // Try to get tile information
        game::TileInfo tileInfo;
        if (!game::get_tile_info(thisPtr, *ctx.manifest, tileInfo, ctx.draw.CRes_Demand)) {
            // Can't resolve tile info - use original function
            H_RenderTexture.original()(thisPtr, texId, unused, x, y, flags);
            return;
        }

        // Bounds check the tile index before accessing
        if (tileInfo.index < 0) {
            H_RenderTexture.original()(thisPtr, texId, unused, x, y, flags);
            return;
        }

        const auto &entry = tileInfo.table[tileInfo.index];
        int U0 = entry.u, V0 = entry.v;

        // Detect area scale from authored TIS metadata first, with heuristics as a fallback.
        if (!ctx.scaleDetected.load()) {
            if (const auto headerDetection = game::detect_scale_from_tis_header(tileInfo, *ctx.manifest)) {
                ctx.areaScale.store(headerDetection->scaleFactor);
                ctx.scaleDetected.store(true);

                if (headerDetection->scaleFactor > 1) {
                    LOG_INFO("Detected {}x tiles from TIS header (tileDimension=0x{:X})",
                             headerDetection->scaleFactor,
                             headerDetection->detectedTileDimension);
                } else {
                    try {
                        H_RenderTexture.disable();
                        ctx.isRenderHookActive = false;
                        LOG_INFO("Detected standard tiles from TIS header (tileDimension=0x{:X})",
                                 headerDetection->detectedTileDimension);
                    } catch (const std::exception &e) {
                        LOG_WARN("Failed to disable RenderTexture hook after TIS header detection: {}", e.what());
                    }
                    H_RenderTexture.original()(thisPtr, texId, unused, x, y, flags);
                    return;
                }
            } else if (const auto tableDetection = game::detect_scale_from_tile_table(tileInfo)) {
                ctx.areaScale.store(tableDetection->scaleFactor);
                ctx.scaleDetected.store(true);

                if (tableDetection->scaleFactor > 1) {
                    LOG_INFO("Detected {}x tiles from PVR entry table (smallest step=0x{:X})",
                             tableDetection->scaleFactor,
                             tableDetection->detectedTileDimension);
                } else {
                    try {
                        H_RenderTexture.disable();
                        ctx.isRenderHookActive = false;
                        LOG_INFO("Detected standard tiles from PVR entry table (smallest step=0x{:X})",
                                 tableDetection->detectedTileDimension);
                    } catch (const std::exception &e) {
                        LOG_WARN("Failed to disable RenderTexture hook after table detection: {}", e.what());
                    }
                    H_RenderTexture.original()(thisPtr, texId, unused, x, y, flags);
                    return;
                }
            } else if (ctx.detectionCount.load() < game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
                const int sampleCount = ctx.detectionCount.fetch_add(1) + 1;
                if (sampleCount == 1) {
                    if (const auto headerTileDimension = game::get_tis_header_tile_dimension(tileInfo, *ctx.manifest)) {
                        log_tis_header_diagnostics(thisPtr,
                                                   tileInfo,
                                                   "Unsupported deterministic tile metadata; using heuristic fallback",
                                                   *headerTileDimension,
                                                   true);
                    } else {
                        log_tis_header_diagnostics(thisPtr,
                                                   tileInfo,
                                                   "TIS header missing and tile table did not resolve scale; using heuristic fallback",
                                                   std::nullopt,
                                                   true);
                    }
                }

                if (game::is_upscaled_by_heuristics(tileInfo, texId)) {
                    ctx.areaScale.store(4);
                    ctx.scaleDetected.store(true);
                    LOG_INFO("Upscaled tiles detected via heuristic fallback (sample {}, texId: {}, UV: ({},{}))",
                             sampleCount,
                             texId,
                             entry.u,
                             entry.v);
                } else if (sampleCount == game::UpscaleThresholds::DETECTION_SAMPLE_COUNT) {
                    ctx.areaScale.store(1);
                    ctx.scaleDetected.store(true);

                    try {
                        H_RenderTexture.disable();
                        ctx.isRenderHookActive = false;
                        LOG_INFO("Standard tiles detected via heuristic fallback after {} samples", sampleCount);
                    } catch (const std::exception &e) {
                        LOG_WARN("Failed to disable RenderTexture hook: {}", e.what());
                    }
                    H_RenderTexture.original()(thisPtr, texId, unused, x, y, flags);
                    return;
                }
            } else {
                ctx.areaScale.store(1);
                ctx.scaleDetected.store(true);
            }
        }

        int scaleFactor;
        if (ctx.scaleDetected.load()) {
            // Scale is stable after detection - use cached value with area change detection
            thread_local int cachedScale = 0;
            thread_local uint32_t cachedAreaId = 0;

            uint32_t currentAreaId = g_currentAreaId.load();
            if (cachedAreaId != currentAreaId) {
                cachedScale = ctx.areaScale.load();
                cachedAreaId = currentAreaId;
            }
            scaleFactor = cachedScale;
        } else {
            scaleFactor = ctx.areaScale.load();
        }
        int DU = game::TileDimensions::STANDARD_SIZE * scaleFactor, DV =
                game::TileDimensions::STANDARD_SIZE * scaleFactor;

        // Handle special texture cases
        unsigned long savedColor = 0;
        if (texId == 0) {
            if (ctx.draw.DrawDisable) ctx.draw.DrawDisable(1);
            if (ctx.draw.DrawColor) savedColor = ctx.draw.DrawColor(game::BLACK_COLOR);
        } else if (texId != -1) {
            if (ctx.draw.DrawBindTexture) ctx.draw.DrawBindTexture(texId);
        }

        // Apply texture enhancements to area tiles (detected by successful get_tile_info)
        // Only enhance textures that look like area tiles (higher IDs)
        // UI textures typically have lower IDs and should not be enhanced
        int currentLastTex = ctx.lastTexId.load();
        if (texId != currentLastTex && texId > game::UpscaleThresholds::UI_TEXTURE_THRESHOLD) {
            if (game::ensure_texture_params(ctx.cfg, ctx.draw, texId)) {
                ctx.lastTexId.store(texId);
                LOG_DEBUG_FAST("Enhanced tile texture {}", texId);
            } else {
                LOG_WARN("Failed to configure GL texture parameters for tile texture {}", texId);
            }
        }

        if (ctx.draw.DrawPushState) ctx.draw.DrawPushState();

        int tone = static_cast<int>(ShaderTone::Seam);

        // Check if grey tone is specifically requested via flags
        if (flags & game::RenderFlags::GREY_TONE_MASK) {
            tone = static_cast<int>(ShaderTone::Grey);
        }

        // Check the "linear tiles" switch in TIS structure
        if (game::get_tis_linear_tiles_flag(tileInfo.tileset, *ctx.manifest)) {
            tone = static_cast<int>(ShaderTone::Seam);
        }

        if (ctx.draw.DrawColorTone) ctx.draw.DrawColorTone(tone);

        if (ctx.draw.DrawBegin) ctx.draw.DrawBegin(static_cast<int>(DrawMode::Triangles));

        // Keep the 64×64 screen quad (lighting/scissor correctness)
        const int X0 = x, Y0 = y;
        const int X1 = x + game::TileDimensions::RENDER_QUAD_SIZE, Y1 = y + game::TileDimensions::RENDER_QUAD_SIZE;

        // Triangle 1
        if (ctx.draw.DrawTexCoord && ctx.draw.DrawVertex) {
            ctx.draw.DrawTexCoord(U0, V0);
            ctx.draw.DrawVertex(X0, Y0);
            ctx.draw.DrawTexCoord(U0, V0 + DV);
            ctx.draw.DrawVertex(X0, Y1);
            ctx.draw.DrawTexCoord(U0 + DU, V0);
            ctx.draw.DrawVertex(X1, Y0);

            // Triangle 2
            ctx.draw.DrawTexCoord(U0 + DU, V0);
            ctx.draw.DrawVertex(X1, Y0);
            ctx.draw.DrawTexCoord(U0, V0 + DV);
            ctx.draw.DrawVertex(X0, Y1);
            ctx.draw.DrawTexCoord(U0 + DU, V0 + DV);
            ctx.draw.DrawVertex(X1, Y1);
        }

        if (ctx.draw.DrawEnd) ctx.draw.DrawEnd();
        if (texId == 0 && ctx.draw.DrawColor) ctx.draw.DrawColor(savedColor);
        if (ctx.draw.DrawPopState) ctx.draw.DrawPopState();
    }


    bool install_all(AppContext &ctx) {
        g_ctx = &ctx;

        try {
            H_LoadArea.create(reinterpret_cast<void *>(ctx.addrs.LoadArea), (void *) &Detour_LoadArea);
            LOG_INFO("LoadArea hook created");

            H_RenderTexture.create(reinterpret_cast<void *>(ctx.addrs.RenderTexture), (void *) &Detour_RenderTexture);
            LOG_INFO("RenderTexture hook created");

            if (ctx.draw.DrawColorTone) {
                try {
                    H_DrawColorTone.create(reinterpret_cast<void *>(ctx.draw.DrawColorTone),
                                           (void *) &Detour_DrawColorTone);
                    H_DrawColorTone.enable();
                    LOG_INFO("DrawColorTone hook installed");
                } catch (const std::exception &e) {
                    LOG_WARN("Failed to create DrawColorTone hook: {} - continuing without it", e.what());
                }
            }

            H_LoadArea.enable();
            LOG_INFO("LoadArea hook enabled");

            H_RenderTexture.enable();
            LOG_INFO("RenderTexture hook enabled");

            ctx.isRenderHookActive = true;

            LOG_INFO("All hooks installed successfully");
            LOG_INFO("LoadArea: 0x{:X}", ctx.addrs.LoadArea);
            LOG_INFO("RenderTexture: 0x{:X}", ctx.addrs.RenderTexture);
            LOG_INFO("RenderTexture hook enabled - will detect upscaled textures automatically");

            return true;
        } catch (const std::exception &e) {
            LOG_ERROR("Exception during hook installation: {}", e.what());
            return false;
        } catch (...) {
            LOG_ERROR("Unknown exception during hook installation");
            return false;
        }
    }

    void uninstall_all() {
        LOG_INFO("Uninstalling all hooks...");

        H_DrawColorTone.disable();
        H_RenderTexture.disable();
        H_LoadArea.disable();

        g_ctx = nullptr;

        LOG_INFO("Hook cleanup complete");
    }

    void prepare_for_shutdown() noexcept {
        if (g_ctx) {
            g_ctx->isRenderHookActive = false;
        }

        H_DrawColorTone.disable();
        H_RenderTexture.disable();
        H_LoadArea.disable();

        g_ctx = nullptr;
    }

    bool is_active() {
        return g_ctx != nullptr && g_ctx->isRenderHookActive;
    }
}
