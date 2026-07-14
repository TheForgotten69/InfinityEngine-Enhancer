#pragma once

#include <cstdint>
#include <vector>

#include "resref_runtime.h"
#include "tile_liquid.h"
#include "tis_runtime.h"

namespace iee::game {
inline constexpr std::size_t kMaxTintTileCandidatesPerOverlay = 128;

struct WedOverlayInfo {
  std::uint16_t width{};
  std::uint16_t height{};
  ResrefBuffer tilesetResref{};
  std::uint32_t tilemapOffset{};
  std::uint32_t tileIndexLookupOffset{};
  TileLiquidMode liquidMode{TileLiquidMode::None};
  std::uint32_t coverageCells{};
  // Bounded unique tile indices used only for authored liquid-tint sampling.
  // Keeping per-cell indices would permit millions of entries per overlay even
  // though the runtime consumes only a small sample.
  std::vector<std::uint16_t> tintTileCandidates{};

  [[nodiscard]] std::string_view tilesetResrefView() const noexcept;
};

struct WedAreaInfo {
  ResrefBuffer areaResref{};
  std::uint32_t overlayCount{};
  std::uint16_t baseWidth{};
  std::uint16_t baseHeight{};
  std::vector<WedOverlayInfo> overlays{};
  std::vector<std::uint8_t> baseOverlayFlags{};

  [[nodiscard]] bool empty() const noexcept { return overlays.empty(); }
  [[nodiscard]] std::string_view areaResrefView() const noexcept;
};

[[nodiscard]] bool parse_loaded_wed(const CRes& resource, WedAreaInfo& out) noexcept;

[[nodiscard]] std::uint8_t liquid_overlay_mask(const WedAreaInfo& wed) noexcept;
}  // namespace iee::game
