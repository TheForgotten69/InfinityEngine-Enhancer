#include "area_texture.h"

#include "tile_liquid.h"

namespace iee::game {
std::optional<AreaCellTexture> pack_area_liquid_texture(const WedAreaInfo& wed) {
  const std::size_t expected = std::size_t{wed.baseWidth} * wed.baseHeight;
  if (expected == 0 || wed.baseOverlayFlags.size() != expected) {
    return std::nullopt;
  }
  AreaCellTexture out{};
  out.width = wed.baseWidth;
  out.height = wed.baseHeight;
  out.texels.resize(expected, 0);
  for (std::size_t cell = 0; cell < expected; ++cell) {
    const std::uint8_t flags = wed.baseOverlayFlags[cell];
    for (std::size_t overlay = 1; overlay < wed.overlays.size() && overlay <= 7; ++overlay) {
      if ((flags & (1u << overlay)) == 0) continue;
      const auto mode = wed.overlays[overlay].liquidMode;
      if (mode != TileLiquidMode::None) {
        out.texels[cell] = static_cast<std::uint8_t>(mode);
        break;  // lowest overlay index wins
      }
    }
  }
  return out;
}
}  // namespace iee::game
