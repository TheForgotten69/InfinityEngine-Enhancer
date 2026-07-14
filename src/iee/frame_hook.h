#pragma once

namespace iee::frame {
    // Hooks SDL_GL_SwapWindow when dynamically available, otherwise the
    // validated gdi32!SwapBuffers fallback. Returns false only if neither frame
    // boundary can be installed.
    bool install(bool enablePerformanceLogging);

    void uninstall() noexcept;

    unsigned long long frame_count() noexcept;

    bool boundary_available() noexcept;

    float seconds_since_install() noexcept;
}
