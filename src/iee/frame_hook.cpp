#include "frame_hook.h"

#include <atomic>
#include <windows.h>

#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/shader_probe.h"

namespace iee::frame {
    namespace {
        using Fn_SwapWindow = void (*)(void *);

        core::Hook<Fn_SwapWindow> g_swapHook;
        std::atomic<unsigned long long> g_frames{0};
        LARGE_INTEGER g_freq{};
        LARGE_INTEGER g_start{};

        void detour_swap(void *window) {
            g_frames.fetch_add(1, std::memory_order_relaxed);
            probe::on_frame_tick(seconds_since_install());
            g_swapHook.original()(window);
        }
    }

    bool install() {
        const HMODULE sdl = GetModuleHandleA("SDL2.dll");
        if (!sdl) {
            LOG_WARN("SDL2.dll not loaded; frame hook unavailable");
            return false;
        }
        const auto target = reinterpret_cast<void *>(GetProcAddress(sdl, "SDL_GL_SwapWindow"));
        if (!target) {
            LOG_WARN("SDL_GL_SwapWindow export not found");
            return false;
        }

        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_start);

        try {
            g_swapHook.create(target, reinterpret_cast<void *>(&detour_swap));
            g_swapHook.enable();
            LOG_INFO("Frame hook installed on SDL_GL_SwapWindow");
            return true;
        } catch (const std::exception &e) {
            LOG_WARN("Frame hook install failed: {}", e.what());
            return false;
        }
    }

    void uninstall() noexcept {
        g_swapHook.disable();
    }

    unsigned long long frame_count() noexcept {
        return g_frames.load(std::memory_order_relaxed);
    }

    float seconds_since_install() noexcept {
        if (g_freq.QuadPart == 0) {
            return 0.0f;
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<float>(static_cast<double>(now.QuadPart - g_start.QuadPart) /
                                  static_cast<double>(g_freq.QuadPart));
    }
}
