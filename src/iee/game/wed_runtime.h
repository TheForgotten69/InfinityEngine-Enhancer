#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "game_types.h"
#include "resref_runtime.h"
#include "tile_liquid.h"
#include "tis_runtime.h"

namespace iee::game {
    struct WedOverlayInfo {
        std::uint16_t width{};
        std::uint16_t height{};
        ResrefBuffer tilesetResref{};
        std::uint16_t uniqueTileCount{};
        std::uint16_t movementType{};
        std::uint32_t tilemapOffset{};
        std::uint32_t tileIndexLookupOffset{};
        TileLiquidMode liquidMode{TileLiquidMode::None};
        std::uint32_t coverageCells{};

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

    [[nodiscard]] bool parse_loaded_wed(const CRes &resource, WedAreaInfo &out) noexcept;

    [[nodiscard]] std::uint8_t liquid_overlay_mask(const WedAreaInfo &wed) noexcept;

    [[nodiscard]] bool base_cell_has_liquid_overlay(const WedAreaInfo &wed,
                                                    std::size_t cellIndex,
                                                    std::uint8_t &outOverlayFlags) noexcept;

    [[nodiscard]] std::optional<std::size_t> base_cell_index_from_screen_point(const WedAreaInfo &wed,
                                                                                int screenX,
                                                                                int screenY,
                                                                                int worldOffsetX,
                                                                                int worldOffsetY) noexcept;
}
