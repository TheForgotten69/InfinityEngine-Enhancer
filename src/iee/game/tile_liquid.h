#pragma once

#include <string_view>

namespace iee::game {
enum class TileLiquidMode : int {
  None = 0,
  Water = 1,
  Lava = 2,
  Goo = 3,
  Sewage = 4,
  Swamp = 5,
};

[[nodiscard]] std::string_view tile_liquid_mode_name(TileLiquidMode mode) noexcept;

[[nodiscard]] TileLiquidMode classify_liquid_tileset(std::string_view resref) noexcept;
}  // namespace iee::game
