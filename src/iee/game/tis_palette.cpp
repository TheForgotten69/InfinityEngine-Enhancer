#include "tis_palette.h"

namespace iee::game {
    std::optional<TileAlpha> decode_palette_tile_alpha(const std::uint8_t *data, std::size_t size) {
        if (!data || size < kPaletteTileBytes) {
            return std::nullopt;
        }

        // Palette entries are BGRA.
        std::array<std::uint8_t, 256> entryTransparent{};
        for (std::size_t i = 0; i < 256; ++i) {
            const std::uint8_t b = data[i * 4 + 0];
            const std::uint8_t g = data[i * 4 + 1];
            const std::uint8_t r = data[i * 4 + 2];
            const bool greenKey = (r == 0 && g == 255 && b == 0);
            entryTransparent[i] = (greenKey || i == 0) ? 1 : 0;
        }

        TileAlpha out{};
        const std::uint8_t *indices = data + 1024;
        for (std::size_t i = 0; i < out.opaque.size(); ++i) {
            out.opaque[i] = entryTransparent[indices[i]] ? 0 : 1;
        }
        return out;
    }

    std::optional<std::array<float, 3>> palette_tile_average_color(const std::uint8_t *data,
                                                                   std::size_t size) {
        const auto alpha = decode_palette_tile_alpha(data, size);
        if (!alpha) {
            return std::nullopt;
        }

        const std::uint8_t *indices = data + 1024;
        double sumR = 0.0, sumG = 0.0, sumB = 0.0;
        std::size_t opaqueCount = 0;
        for (std::size_t i = 0; i < alpha->opaque.size(); ++i) {
            if (!alpha->opaque[i]) {
                continue;
            }
            const std::size_t entry = indices[i];
            sumB += data[entry * 4 + 0];
            sumG += data[entry * 4 + 1];
            sumR += data[entry * 4 + 2];
            ++opaqueCount;
        }
        if (opaqueCount == 0) {
            return std::nullopt;
        }
        const auto scale = 1.0 / (255.0 * static_cast<double>(opaqueCount));
        return std::array<float, 3>{static_cast<float>(sumR * scale),
                                    static_cast<float>(sumG * scale),
                                    static_cast<float>(sumB * scale)};
    }
}
