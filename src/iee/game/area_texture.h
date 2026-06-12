#pragma once
#include <cstdint>
#include <optional>
#include <vector>

#include "wed_runtime.h"

namespace iee::game {
    struct AreaCellTexture {
        int width{};
        int height{};
        std::vector<std::uint8_t> texels; // R8, one texel per WED base cell, row-major
    };

    // Packs the per-cell overlay flags of a parsed WED into an R8 grid suitable
    // for upload as a GL texture (one texel per base cell).
    [[nodiscard]] std::optional<AreaCellTexture> pack_area_cell_texture(const WedAreaInfo &wed);
}
