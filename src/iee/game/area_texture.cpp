#include "area_texture.h"

namespace iee::game {
    std::optional<AreaCellTexture> pack_area_cell_texture(const WedAreaInfo &wed) {
        const std::size_t expected = std::size_t{wed.baseWidth} * wed.baseHeight;
        if (expected == 0 || wed.baseOverlayFlags.size() != expected) {
            return std::nullopt;
        }
        AreaCellTexture out{.width = wed.baseWidth, .height = wed.baseHeight};
        out.texels = wed.baseOverlayFlags;
        return out;
    }
}
