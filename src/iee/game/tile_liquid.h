#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>

#include "tis_runtime.h"

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

    [[nodiscard]] TileLiquidMode classify_liquid_tileset(const TileInfo &tileInfo) noexcept;

    enum class LiquidAtlasClass : int {
        None = 0,
        OpenWater = 1,
        BasinWater = 2,
    };

    [[nodiscard]] std::string_view liquid_atlas_class_name(LiquidAtlasClass atlasClass) noexcept;

    [[nodiscard]] LiquidAtlasClass classify_liquid_atlas_key(std::string_view tilesetResref,
                                                             int tileIndex,
                                                             int page,
                                                             int u,
                                                             int v) noexcept;

    [[nodiscard]] LiquidAtlasClass classify_liquid_atlas_key(const TileInfo &tileInfo) noexcept;

    struct LiquidMaskGrid {
        int columns{};
        int rows{};
        std::vector<std::uint8_t> cells{};

        [[nodiscard]] bool any() const noexcept;
        [[nodiscard]] std::size_t count() const noexcept;
        [[nodiscard]] bool at(int column, int row) const noexcept;
    };

    [[nodiscard]] bool liquid_color_matches(TileLiquidMode mode,
                                            std::uint8_t r,
                                            std::uint8_t g,
                                            std::uint8_t b,
                                            std::uint8_t a) noexcept;

    [[nodiscard]] LiquidMaskGrid build_liquid_mask_grid(TileLiquidMode mode,
                                                        const std::uint8_t *rgba,
                                                        int textureWidth,
                                                        int textureHeight,
                                                        int u0,
                                                        int v0,
                                                        int du,
                                                        int dv,
                                                        int columns = 8,
                                                        int rows = 8) noexcept;

    [[nodiscard]] LiquidMaskGrid build_liquid_atlas_mask_grid(LiquidAtlasClass atlasClass,
                                                              int columns = 8,
                                                              int rows = 8) noexcept;

    enum class LiquidBaseReplacementPath : int {
        None = 0,
        AtlasKeyed = 1,
        Heuristic = 2,
    };

    [[nodiscard]] LiquidBaseReplacementPath select_liquid_base_replacement(bool wedCovered,
                                                                           LiquidAtlasClass atlasClass,
                                                                           bool heuristicMaskAny) noexcept;
}
