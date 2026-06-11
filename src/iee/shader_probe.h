#pragma once
#include <string_view>
#include "iee/core/config.h"

namespace iee::probe {
    struct ShaderRuntimeCapabilities {
        bool baseGlReady{};
        bool shaderObjectsAvailable{};
        bool shaderIntrospectionAvailable{};
        bool uniformApiAvailable{};
        bool readyForSourcePatching{};
        std::string_view glVersion{};
        std::string_view glslVersion{};
        std::string_view glVendor{};
        std::string_view glRenderer{};
    };

    [[nodiscard]] ShaderRuntimeCapabilities detect_shader_runtime_capabilities() noexcept;
    void log_shader_runtime_capabilities() noexcept;
    bool install_shader_probes(const core::EngineConfig &cfg) noexcept;
    void uninstall_shader_probes() noexcept;
    // Called once per frame by the frame hook (Task 8); advances uIeeTime.
    void on_frame_tick(float secondsSinceStart) noexcept;
    // Hotkey cycle target: uIeeEnabled value 0=off / 1=effect on / 2=alignment debug.
    void set_override_effect_enabled(bool enabled) noexcept;
    [[nodiscard]] bool override_effect_enabled() noexcept;

    // Published by area_state at area load; consumed by the uniform feed.
    void set_area_world_size(float widthPx, float heightPx) noexcept;

    // Published by tile_render per tile draw; consumed by the uniform feed.
    void set_area_scroll_zoom(float scrollX, float scrollY, float zoom) noexcept;
}
