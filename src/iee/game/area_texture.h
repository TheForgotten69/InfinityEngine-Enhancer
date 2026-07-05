#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "tis_palette.h"
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

    // Packs per-cell liquid classification: texel = TileLiquidMode (0..5) of the
    // lowest-index liquid overlay covering the cell, 0 when none. R8, row-major.
    [[nodiscard]] std::optional<AreaCellTexture> pack_area_liquid_texture(const WedAreaInfo &wed);

    // Fine liquid mask: 8 texels per base cell edge (one texel per 8x8 pixel
    // block of the 64px cell). Fetches the covering liquid overlay's tile alpha
    // via the provider; returns nullopt from the provider = undecodable tile.
    inline constexpr int kFineMaskTexelsPerCell = kTilePixels / 8;

    using OverlayTileAlphaProvider =
            std::function<std::optional<TileAlpha>(std::size_t overlayIndex, std::uint16_t tileIndex)>;

    // Stamps per-texel liquid classification from the painted overlay-tile
    // silhouette: texel = TileLiquidMode where the 8x8 pixel block is
    // majority-opaque (>= half), 0 elsewhere. Cells whose overlay tile cannot
    // be resolved or decoded fall back to full-cell liquid (the coarse
    // pack_area_liquid_texture behavior).
    [[nodiscard]] std::optional<AreaCellTexture> build_fine_liquid_mask(const WedAreaInfo &wed,
                                                                        const OverlayTileAlphaProvider &tileAlpha);
}
