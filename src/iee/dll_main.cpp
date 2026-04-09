#include <windows.h>
#include <memory>
#include <thread>

#include "app_context.h"
#include "hooks.h"
#include "iee/game/build_manifest.h"
#include "iee/core/logger.h"
#include "iee/core/config.h"
#include "iee/core/pattern_scanner.h"
#include "iee/game/game_addrs.h"
#include "iee/game/renderer.h"

namespace iee {
    static std::unique_ptr<AppContext> g_appContext;
    const std::string LOG_FILE = "InfinityEngine-Enhancer.log";

    static DWORD WINAPI InitThread(LPVOID) {
        try {
            const auto cfgPath = core::ConfigManager::config_path();
            core::EngineConfig cfg = core::ConfigManager::load_or_default();

            auto logPath = cfgPath.parent_path() / LOG_FILE;
            core::init_logger(logPath.string(), cfg.enableVerboseLogging);

            LOG_INFO("Infinity Engine Enhancer initializing...");

            g_appContext = std::make_unique<AppContext>();
            auto &ctx = *g_appContext;
            ctx.cfg = cfg;
            ctx.manifest = game::detect_manifest();

            LOG_INFO("InfinityEngine-Enhancer loaded (config applied)");
            if (!ctx.manifest) {
                LOG_ERROR("Failed to select a build manifest");
                return 1;
            }
            LOG_INFO("Selected build manifest: {}", ctx.manifest->buildId);

            if (cfg.enableVerboseLogging) {
                LOG_INFO("=== DETAILED DIAGNOSTICS ===");
                LOG_INFO("Texture Enhancements:");
                LOG_INFO("  Linear filtering: ALWAYS ENABLED");
                LOG_INFO("  Anisotropic filtering: {}", ctx.cfg.enableAnisotropicFiltering ? "ENABLED" : "disabled");
                if (ctx.cfg.enableAnisotropicFiltering) {
                    LOG_INFO("    Max anisotropy: {:.1f}x", ctx.cfg.maxAnisotropy);
                }
                LOG_INFO("  LOD bias: {:.2f}", ctx.cfg.lodBias);
                LOG_INFO("Cached addresses: LoadArea=0x{:X}, RenderTexture=0x{:X}",
                         ctx.cfg.cachedLoadAreaRVA, ctx.cfg.cachedRenderTextureRVA);
                LOG_INFO("=============================");
            }


            if (!game::resolve_addresses(ctx.addrs, ctx.cfg, *ctx.manifest)) {
                LOG_ERROR("Critical error: Failed to locate game functions");
                return 1;
            }

            if (auto moduleInfo = core::get_module_span(nullptr)) {
                LOG_DEBUG("Base=0x{:X}  LoadArea=0x{:X}  RenderTexture=0x{:X}",
                          (uintptr_t)moduleInfo->base,
                          ctx.addrs.LoadArea, ctx.addrs.RenderTexture);
            }

            if (!game::resolve_draw_api(ctx.draw, ctx.addrs.RenderTexture, *ctx.manifest)) {
                LOG_ERROR("Failed to resolve draw API functions");
                return 1;
            }

            if (!hooks::install_all(ctx)) {
                LOG_ERROR("Failed to install hooks");
                return 1;
            }

            LOG_DEBUG("Installation complete");

            return 0;
        } catch (const std::exception &e) {
            LOG_ERROR("Exception during initialization: {}", e.what());
            return 1;
        } catch (...) {
            LOG_ERROR("Unknown exception during initialization");
            return 1;
        }
    }

    static void CleanupHooks() {
        if (g_appContext) {
            hooks::prepare_for_shutdown();
            g_appContext->reset_all_state();
            g_appContext.reset();
        }
    }
}

// EEex integration
extern "C" __declspec(dllexport) void __stdcall InitBindings(void *argSharedState) {
    (void) argSharedState;
    if (HANDLE th = CreateThread(nullptr, 0, iee::InitThread, nullptr, 0, nullptr)) {
        CloseHandle(th);
    } else {
        OutputDebugStringA("InfinityEngine-Enhancer: Failed to create initialization thread");
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    switch (r) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(h);
            break;

        case DLL_PROCESS_DETACH:
            iee::CleanupHooks();
            break;
        default: break;
    }
    return TRUE;
}
