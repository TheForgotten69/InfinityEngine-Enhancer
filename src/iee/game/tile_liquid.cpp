#include "tile_liquid.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace iee::game {
namespace {

std::string upper_copy(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return result;
}

bool starts_with_any(std::string_view value,
                     std::initializer_list<std::string_view> prefixes) noexcept {
  for (const auto prefix : prefixes) {
    if (value.starts_with(prefix)) return true;
  }
  return false;
}

}  // namespace

std::string_view tile_liquid_mode_name(TileLiquidMode mode) noexcept {
  switch (mode) {
    case TileLiquidMode::Water:
      return "water";
    case TileLiquidMode::Lava:
      return "lava";
    case TileLiquidMode::Goo:
      return "goo";
    case TileLiquidMode::Sewage:
      return "sewage";
    case TileLiquidMode::Swamp:
      return "swamp";
    case TileLiquidMode::None:
    default:
      return "none";
  }
}

TileLiquidMode classify_liquid_tileset(std::string_view resref) noexcept {
  if (resref.empty()) return TileLiquidMode::None;

  const auto upper = upper_copy(resref);
  if (starts_with_any(upper, {"WTLAVA"})) return TileLiquidMode::Lava;
  if (starts_with_any(upper, {"WTGOO"})) return TileLiquidMode::Goo;
  if (starts_with_any(upper, {"WTSEW"})) return TileLiquidMode::Sewage;
  if (starts_with_any(upper, {"WTSW"})) return TileLiquidMode::Swamp;
  if (starts_with_any(upper, {"WTWAVE", "WTRIV", "WTPOOL", "WTLAK", "WTFALL", "WTURN", "YSPOOL",
                              "YSRIV", "YSWAVE"})) {
    return TileLiquidMode::Water;
  }
  return TileLiquidMode::None;
}

}  // namespace iee::game
