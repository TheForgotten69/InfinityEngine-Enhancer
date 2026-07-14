#include "frame_hook.h"

#include <windows.h>

#include <atomic>
#include <exception>

#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/shader_probe.h"

namespace iee::frame {
namespace {
using Fn_SdlSwapWindow = void (*)(void*);
using Fn_SwapBuffers = BOOL(WINAPI*)(HDC);

core::Hook<Fn_SdlSwapWindow> g_sdlSwapHook;
core::Hook<Fn_SwapBuffers> g_gdiSwapHook;
std::atomic<unsigned long long> g_frames{0};
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_start{};

void frame_tick() {
  g_frames.fetch_add(1, std::memory_order_relaxed);
  probe::on_frame_tick(seconds_since_install());
}

void detour_sdl_swap(void* window) {
  frame_tick();
  g_sdlSwapHook.original()(window);
}

BOOL WINAPI detour_gdi_swap(HDC hdc) {
  frame_tick();
  return g_gdiSwapHook.original()(hdc);
}

bool install_sdl_hook() {
  const HMODULE sdl = GetModuleHandleA("SDL2.dll");
  if (!sdl) {
    return false;
  }
  const auto target = reinterpret_cast<void*>(GetProcAddress(sdl, "SDL_GL_SwapWindow"));
  if (!target) {
    return false;
  }
  g_sdlSwapHook.create(target, reinterpret_cast<void*>(&detour_sdl_swap));
  g_sdlSwapHook.enable();
  LOG_INFO("Frame hook installed on SDL2!SDL_GL_SwapWindow");
  return true;
}

bool install_gdi_hook() {
  // BGEE statically links SDL (confirmed live: no SDL2.dll module).
  // gdi32!SwapBuffers is the universal WGL frame boundary: both
  // SDL's GL backend and direct wglSwapBuffers route through it.
  const HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
  if (!gdi32) {
    return false;
  }
  const auto target = reinterpret_cast<void*>(GetProcAddress(gdi32, "SwapBuffers"));
  if (!target) {
    return false;
  }
  g_gdiSwapHook.create(target, reinterpret_cast<void*>(&detour_gdi_swap));
  g_gdiSwapHook.enable();
  LOG_INFO("Frame hook installed on gdi32!SwapBuffers");
  return true;
}
}  // namespace

bool install() {
  QueryPerformanceFrequency(&g_freq);
  QueryPerformanceCounter(&g_start);

  try {
    if (install_sdl_hook()) {
      return true;
    }
    if (install_gdi_hook()) {
      return true;
    }
    LOG_WARN(
        "Frame hook unavailable: neither SDL2!SDL_GL_SwapWindow nor gdi32!SwapBuffers resolved");
    return false;
  } catch (const std::exception& e) {
    LOG_WARN("Frame hook install failed: {}", e.what());
    (void)g_sdlSwapHook.remove();
    (void)g_gdiSwapHook.remove();
    return false;
  } catch (...) {
    LOG_WARN("Frame hook install failed with an unknown exception");
    (void)g_sdlSwapHook.remove();
    (void)g_gdiSwapHook.remove();
    return false;
  }
}

void uninstall() noexcept {
  (void)g_sdlSwapHook.remove();
  (void)g_gdiSwapHook.remove();
}

unsigned long long frame_count() noexcept { return g_frames.load(std::memory_order_relaxed); }

float seconds_since_install() noexcept {
  if (g_freq.QuadPart == 0) {
    return 0.0f;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return static_cast<float>(static_cast<double>(now.QuadPart - g_start.QuadPart) /
                            static_cast<double>(g_freq.QuadPart));
}
}  // namespace iee::frame
