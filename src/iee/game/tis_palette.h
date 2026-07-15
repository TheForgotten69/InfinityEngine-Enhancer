#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace iee::game {
    // Classic (palette) TIS tile: 256-entry BGRA palette (1024 bytes) followed
    // by 64x64 one-byte indices — 5120 bytes total.
    inline constexpr std::size_t kPaletteTileBytes = 5120;
    inline constexpr int kTilePixels = 64;

    struct TileAlpha {
        // 1 = opaque (liquid art), 0 = transparent (land shows through).
        std::array<std::uint8_t, kTilePixels * kTilePixels> opaque{};
    };

    struct PaletteTileAverage {
        std::array<float, 3> linearRgb{};
        std::size_t opaquePixelCount{};
    };

    // Decodes the transparency of a classic palette tile. Transparent pixels
    // are the overlay key: palette index 0, or any entry that is pure green
    // (0,255,0 — the classic IE transparency color). Returns nullopt when the
    // buffer is not a palette tile.
    [[nodiscard]] std::optional<TileAlpha> decode_palette_tile_alpha(const std::uint8_t *data,
                                                                     std::size_t size);

    // Average RGB (linear 0..1) and opaque-pixel weight of a classic palette
    // tile — the authored color identity of a liquid overlay tile (sea teal,
    // sewer green, ...). Same transparency rule as decode_palette_tile_alpha.
    // Returns nullopt when the buffer is not a palette tile or has no opaque
    // pixels.
    [[nodiscard]] std::optional<PaletteTileAverage> palette_tile_average_color(
        const std::uint8_t *data, std::size_t size);
}
