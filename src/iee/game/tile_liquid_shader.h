#pragma once

#include <string>
#include <string_view>

namespace iee::game {
    struct TileLiquidShaderPatchResult {
        bool patched{};
        std::string source;
    };

    [[nodiscard]] TileLiquidShaderPatchResult patch_fpseam_liquid_fragment_shader(std::string_view source);
}
