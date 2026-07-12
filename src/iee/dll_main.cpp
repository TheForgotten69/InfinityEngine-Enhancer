#include <windows.h>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

#include "app_context.h"
#include "area_state.h"
#include "frame_hook.h"
#include "hooks.h"
#include "iee/shader_probe.h"
#include "iee/game/build_manifest.h"
#include "iee/core/logger.h"
#include "iee/core/config.h"
#include "iee/core/pattern_scanner.h"
#include "iee/game/game_addrs.h"
#include "iee/game/renderer.h"
#include "water_textures.h"

namespace iee {
    static std::unique_ptr<AppContext> g_appContext;
    enum class LifecycleState : std::uint8_t { NotStarted, Starting, Running, Failed, Stopping, Stopped };
    static std::atomic<LifecycleState> g_lifecycle{LifecycleState::NotStarted};
    static std::atomic<HANDLE> g_initThread{nullptr};
    const std::string LOG_FILE = "InfinityEngine-Enhancer.log";
    static void CleanupHooks() noexcept;

    static DWORD WINAPI InitThread(LPVOID) {
        struct FinalizeState {
            bool success{};
            ~FinalizeState() {
                if (!success) CleanupHooks();
                g_lifecycle.store(success ? LifecycleState::Running : LifecycleState::Failed,
                                  std::memory_order_release);
            }
        } finalize;

        try {
            const auto cfgPath = core::ConfigManager::config_path();
            core::EngineConfig cfg = core::ConfigManager::load_or_default();

            auto logPath = cfgPath.parent_path() / LOG_FILE;
            core::init_logger(logPath.string(), cfg.enableVerboseLogging);

            LOG_INFO("Infinity Engine Enhancer initializing...");

            g_appContext = std::make_unique<AppContext>();
            auto &ctx = *g_appContext;
            ctx.cfg = cfg;
            game::ExecutableVersion detectedVersion{};
            ctx.manifest = game::detect_manifest(&detectedVersion);

            LOG_INFO("InfinityEngine-Enhancer loaded (config applied)");
            if (!ctx.manifest) {
                if (detectedVersion.major != 0) {
                    LOG_ERROR("Unsupported executable version {}.{}.{}; no hooks were installed", detectedVersion.major,
                              detectedVersion.minor, detectedVersion.patch);
                } else {
                    LOG_ERROR("Could not read the executable version; no hooks were installed");
                }
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
                LOG_INFO("=============================");
            }

            if (!game::resolve_addresses(ctx.addrs, ctx.cfg, *ctx.manifest)) {
                LOG_ERROR("Critical error: Failed to locate game functions");
                return 1;
            }

            if (auto moduleInfo = core::get_module_span(nullptr)) {
                LOG_DEBUG("Base=0x{:X}  LoadArea=0x{:X}  RenderTexture=0x{:X}",
                          reinterpret_cast<std::uintptr_t>(moduleInfo->base),
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

            // Frame boundary: SDL2 export, available without a GL context.
            // Failure is non-fatal (time-driven shader effects stay at t=0).
            (void)frame::install();

            LOG_DEBUG("Installation complete");

            finalize.success = true;
            return 0;
        } catch (const std::exception &e) {
            try {
                LOG_ERROR("Exception during initialization: {}", e.what());
            } catch (...) {
                OutputDebugStringA("InfinityEngine-Enhancer: exception during initialization");
            }
            return 1;
        } catch (...) {
            try {
                LOG_ERROR("Unknown exception during initialization");
            } catch (...) {
                OutputDebugStringA("InfinityEngine-Enhancer: unknown exception during initialization");
            }
            return 1;
        }
    }

    static void CleanupHooks() noexcept {
        if (g_appContext) {
            frame::uninstall();
            probe::uninstall_shader_probes();
            area::release_gpu_area_resources();
            water::release_water_textures();
            hooks::prepare_for_shutdown();
            g_appContext->reset_all_state();
            g_appContext.reset();
        }
    }
} // namespace iee

// EEex integration
extern "C" __declspec(dllexport) void __stdcall InitBindings(void *argSharedState) {
    (void)argSharedState;
    auto expected = iee::LifecycleState::NotStarted;
    if (!iee::g_lifecycle.compare_exchange_strong(expected, iee::LifecycleState::Starting, std::memory_order_acq_rel)) {
        return;
    }

    if (HANDLE thread = CreateThread(nullptr, 0, iee::InitThread, nullptr, 0, nullptr)) {
        iee::g_initThread.store(thread, std::memory_order_release);
    } else {
        iee::g_lifecycle.store(iee::LifecycleState::Failed, std::memory_order_release);
        OutputDebugStringA("InfinityEngine-Enhancer: Failed to create initialization thread");
    }
}

// EEex or another loader must call this before FreeLibrary. It deliberately
// does not run from DllMain, where MinHook, mutexes, logging, and GL calls are
// unsafe under the Windows loader lock.
extern "C" __declspec(dllexport) void __stdcall ShutdownBindings() {
    const auto previous = iee::g_lifecycle.exchange(iee::LifecycleState::Stopping, std::memory_order_acq_rel);
    if (previous == iee::LifecycleState::Stopping) {
        return;
    }
    if (previous == iee::LifecycleState::NotStarted || previous == iee::LifecycleState::Stopped) {
        iee::g_lifecycle.store(iee::LifecycleState::Stopped, std::memory_order_release);
        return;
    }

    if (HANDLE thread = iee::g_initThread.exchange(nullptr, std::memory_order_acq_rel)) {
        if (GetThreadId(thread) != GetCurrentThreadId()) {
            WaitForSingleObject(thread, INFINITE);
        }
        CloseHandle(thread);
    }
    iee::CleanupHooks();
    iee::g_lifecycle.store(iee::LifecycleState::Stopped, std::memory_order_release);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID reserved) {
    switch (r) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(h);
        break;

    case DLL_PROCESS_DETACH:
        if (reserved == nullptr && iee::g_lifecycle.load(std::memory_order_acquire) != iee::LifecycleState::Stopped) {
            OutputDebugStringA("InfinityEngine-Enhancer: FreeLibrary called without ShutdownBindings; skipping unsafe "
                               "loader-lock cleanup");
        }
        break;
    default:
        break;
    }
    return TRUE;
}
