#pragma once

#include "iee/core/config.h"

#include <string_view>

namespace iee::game {
    struct ShaderRenderContext {
        bool active{};
        int texId{-1};
        unsigned long flags{};
        int tone{-1};
        std::string_view areaResref{};
    };

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
    bool install_shader_probes(const core::EngineConfig& cfg) noexcept;
    void uninstall_shader_probes() noexcept;
    void set_shader_render_context(const ShaderRenderContext& context) noexcept;
    void clear_shader_render_context() noexcept;
}
