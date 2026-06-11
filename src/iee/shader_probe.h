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
    // Hotkey toggle target (Task 9): flips the value fed to uIeeEnabled.
    void set_override_effect_enabled(bool enabled) noexcept;
    [[nodiscard]] bool override_effect_enabled() noexcept;
}
