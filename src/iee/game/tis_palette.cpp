#include "tis_palette.h"

#include <cmath>

namespace iee::game {
    namespace {
        double srgb_to_linear(std::uint8_t encoded) noexcept {
            const double value = static_cast<double>(encoded) / 255.0;
            return value <= 0.04045 ? value / 12.92
                                    : std::pow((value + 0.055) / 1.055, 2.4);
        }

        bool palette_entry_is_transparent(const std::uint8_t *data,
                                          std::size_t entry) noexcept {
            const std::uint8_t b = data[entry * 4 + 0];
            const std::uint8_t g = data[entry * 4 + 1];
            const std::uint8_t r = data[entry * 4 + 2];
            return entry == 0 || (r == 0 && g == 255 && b == 0);
        }
    }

    std::optional<TileAlpha> decode_palette_tile_alpha(const std::uint8_t *data, std::size_t size) {
        if (!data || size < kPaletteTileBytes) {
            return std::nullopt;
        }

        // Palette entries are BGRA.
        std::array<std::uint8_t, 256> entryTransparent{};
        for (std::size_t i = 0; i < 256; ++i) {
            entryTransparent[i] = palette_entry_is_transparent(data, i) ? 1 : 0;
        }

        TileAlpha out{};
        const std::uint8_t *indices = data + 1024;
        for (std::size_t i = 0; i < out.opaque.size(); ++i) {
            out.opaque[i] = entryTransparent[indices[i]] ? 0 : 1;
        }
        return out;
    }

    std::optional<PaletteTileAverage> palette_tile_average_color(const std::uint8_t *data,
                                                                 std::size_t size) {
        if (!data || size < kPaletteTileBytes) {
            return std::nullopt;
        }

        // Convert each palette entry once. The previous pixel-at-a-time path
        // performed up to three pow() calls for every one of 4096 pixels.
        std::array<std::array<double, 3>, 256> linearPalette{};
        std::array<std::uint8_t, 256> entryTransparent{};
        for (std::size_t entry = 0; entry < linearPalette.size(); ++entry) {
            entryTransparent[entry] = palette_entry_is_transparent(data, entry) ? 1 : 0;
            if (entryTransparent[entry]) continue;
            linearPalette[entry] = {
                srgb_to_linear(data[entry * 4 + 2]),
                srgb_to_linear(data[entry * 4 + 1]),
                srgb_to_linear(data[entry * 4 + 0]),
            };
        }

        const std::uint8_t *indices = data + 1024;
        double sumR = 0.0, sumG = 0.0, sumB = 0.0;
        std::size_t opaqueCount = 0;
        for (std::size_t i = 0; i < kTilePixels * kTilePixels; ++i) {
            const std::size_t entry = indices[i];
            if (entryTransparent[entry]) continue;
            sumR += linearPalette[entry][0];
            sumG += linearPalette[entry][1];
            sumB += linearPalette[entry][2];
            ++opaqueCount;
        }
        if (opaqueCount == 0) {
            return std::nullopt;
        }
        const auto scale = 1.0 / static_cast<double>(opaqueCount);
        return PaletteTileAverage{
            .linearRgb = {static_cast<float>(sumR * scale),
                          static_cast<float>(sumG * scale),
                          static_cast<float>(sumB * scale)},
            .opaquePixelCount = opaqueCount,
        };
    }
}
