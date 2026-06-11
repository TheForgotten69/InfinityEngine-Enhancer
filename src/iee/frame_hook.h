#pragma once

namespace iee::frame {
    // Hooks SDL_GL_SwapWindow from SDL2.dll's export table (stable across game
    // patches; no pattern scan). Returns false if SDL2 or the export is absent.
    bool install();

    void uninstall() noexcept;

    unsigned long long frame_count() noexcept;

    float seconds_since_install() noexcept;
}
