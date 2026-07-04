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
}
