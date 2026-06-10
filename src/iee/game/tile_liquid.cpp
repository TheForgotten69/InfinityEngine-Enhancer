#include "tile_liquid.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace iee::game {
    namespace {
        struct LiquidAtlasSeed {
            std::string_view tileset;
            int page;
            int u0;
            int u1;
            int v0;
            int v1;
            LiquidAtlasClass atlasClass;
        };

        std::string upper_copy(std::string_view value) {
            std::string out(value);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            return out;
        }

        bool starts_with_any(std::string_view value, std::initializer_list<std::string_view> prefixes) noexcept {
            for (const auto prefix: prefixes) {
                if (value.rfind(prefix, 0) == 0) return true;
            }
            return false;
        }

        bool water_like(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
            if (a < 10) return false;
            if (std::max(g, b) < 28) return false;
            if (g + b < static_cast<int>(r) + 44) return false;
            if (g + 8 < r || b + 8 < r) return false;
            if (std::abs(static_cast<int>(g) - static_cast<int>(b)) > 148) return false;
            return true;
        }

        bool lava_like(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
            if (a < 24) return false;
            if (r < 96) return false;
            if (r < g + 20 || g < b) return false;
            return r + g > b + 120;
        }

        bool goo_like(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
            if (a < 24) return false;
            if (g < 72) return false;
            if (g < r + 18 || g < b + 18) return false;
            return true;
        }

        bool sewage_like(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
            if (a < 24) return false;
            if (r < 48 || g < 48) return false;
            if (g + r < static_cast<int>(b) + 64) return false;
            return std::abs(static_cast<int>(r) - static_cast<int>(g)) <= 72;
        }

        bool swamp_like(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
            if (a < 24) return false;
            if (g < 56) return false;
            if (g < r + 10 || g < b + 8) return false;
            return r + g > b + 48;
        }

        LiquidMaskGrid make_empty_mask(int columns, int rows) {
            LiquidMaskGrid mask{};
            mask.columns = std::max(columns, 1);
            mask.rows = std::max(rows, 1);
            mask.cells.assign(static_cast<std::size_t>(mask.columns * mask.rows), 0);
            return mask;
        }

        LiquidMaskGrid make_full_mask(int columns, int rows) {
            auto mask = make_empty_mask(columns, rows);
            std::fill(mask.cells.begin(), mask.cells.end(), static_cast<std::uint8_t>(1));
            return mask;
        }

        LiquidMaskGrid make_basin_mask(int columns, int rows) {
            auto mask = make_empty_mask(columns, rows);
            for (int row = 0; row < mask.rows; ++row) {
                for (int column = 0; column < mask.columns; ++column) {
                    const auto fx = (static_cast<float>(column) + 0.5f) / static_cast<float>(mask.columns);
                    const auto fy = (static_cast<float>(row) + 0.5f) / static_cast<float>(mask.rows);
                    const auto dx = std::abs(fx - 0.5f) * 2.0f;
                    const auto dy = std::abs(fy - 0.5f) * 2.0f;
                    if ((dx + dy) <= 1.28f) {
                        mask.cells[static_cast<std::size_t>(row * mask.columns + column)] = 1;
                    }
                }
            }
            return mask;
        }

        bool atlas_seed_matches(const LiquidAtlasSeed &seed,
                                std::string_view tilesetResref,
                                int page,
                                int u,
                                int v) noexcept {
            return tilesetResref == seed.tileset &&
                   page == seed.page &&
                   u >= seed.u0 &&
                   u < seed.u1 &&
                   v >= seed.v0 &&
                   v < seed.v1;
        }

        constexpr std::array<LiquidAtlasSeed, 5> kLiquidAtlasSeeds{{
            // `A260020.PNG` page 0 top strip: deterministic open-water proof for AR2600 cliffside water.
            {"AR2600", 0, 0, 4096, 0, 256, LiquidAtlasClass::OpenWater},
            // `A260021.PNG` page 1 rows containing fountain / basin interiors.
            {"AR2600", 1, 0, 256, 256, 512, LiquidAtlasClass::BasinWater},
            {"AR2600", 1, 0, 256, 512, 768, LiquidAtlasClass::BasinWater},
            // `A260022.PNG` / `A260023.PNG` expose additional basin-adjacent water fragments.
            {"AR2600", 2, 0, 256, 0, 256, LiquidAtlasClass::BasinWater},
            {"AR2600", 3, 0, 256, 0, 256, LiquidAtlasClass::BasinWater},
        }};

        void prune_small_components(LiquidMaskGrid &mask) {
            if (mask.columns <= 0 || mask.rows <= 0 || mask.cells.empty()) {
                return;
            }

            std::vector<std::uint8_t> visited(mask.cells.size(), 0);
            std::vector<std::size_t> stack;
            std::vector<std::size_t> component;

            for (std::size_t seed = 0; seed < mask.cells.size(); ++seed) {
                if (mask.cells[seed] == 0 || visited[seed] != 0) {
                    continue;
                }

                stack.clear();
                component.clear();
                stack.push_back(seed);
                visited[seed] = 1;

                bool touchesBorder = false;
                while (!stack.empty()) {
                    const auto index = stack.back();
                    stack.pop_back();
                    component.push_back(index);

                    const int row = static_cast<int>(index / static_cast<std::size_t>(mask.columns));
                    const int column = static_cast<int>(index % static_cast<std::size_t>(mask.columns));
                    if (column == 0 || row == 0 || column + 1 == mask.columns || row + 1 == mask.rows) {
                        touchesBorder = true;
                    }

                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            const int nx = column + dx;
                            const int ny = row + dy;
                            if (nx < 0 || ny < 0 || nx >= mask.columns || ny >= mask.rows) continue;
                            const auto neighbor = static_cast<std::size_t>(ny * mask.columns + nx);
                            if (mask.cells[neighbor] == 0 || visited[neighbor] != 0) continue;
                            visited[neighbor] = 1;
                            stack.push_back(neighbor);
                        }
                    }
                }

                const auto size = component.size();
                const bool keep = size >= 4 || (touchesBorder && size >= 2);
                if (keep) continue;

                for (const auto index: component) {
                    mask.cells[index] = 0;
                }
            }
        }
    }

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
        if (starts_with_any(upper, {"WTLAVA"})) {
            return TileLiquidMode::Lava;
        }
        if (starts_with_any(upper, {"WTGOO"})) {
            return TileLiquidMode::Goo;
        }
        if (starts_with_any(upper, {"WTSEW"})) {
            return TileLiquidMode::Sewage;
        }
        if (starts_with_any(upper, {"WTSW"})) {
            return TileLiquidMode::Swamp;
        }
        if (starts_with_any(upper, {"WTWAVE", "WTRIV", "WTPOOL", "WTLAK", "WTFALL", "WTURN", "YSPOOL", "YSRIV", "YSWAVE"})) {
            return TileLiquidMode::Water;
        }

        return TileLiquidMode::None;
    }

    TileLiquidMode classify_liquid_tileset(const TileInfo &tileInfo) noexcept {
        (void) tileInfo;
        // The current TileInfo surface does not carry a tileset resref.
        // Callers that have WED-derived overlay metadata should use the
        // string-view overload directly.
        return TileLiquidMode::None;
    }

    std::string_view liquid_atlas_class_name(LiquidAtlasClass atlasClass) noexcept {
        switch (atlasClass) {
            case LiquidAtlasClass::OpenWater:
                return "open-water";
            case LiquidAtlasClass::BasinWater:
                return "basin-water";
            case LiquidAtlasClass::None:
            default:
                return "none";
        }
    }

    LiquidAtlasClass classify_liquid_atlas_key(std::string_view tilesetResref,
                                               int tileIndex,
                                               int page,
                                               int u,
                                               int v) noexcept {
        (void) tileIndex;
        const auto upper = upper_copy(tilesetResref);
        for (const auto &seed: kLiquidAtlasSeeds) {
            if (atlas_seed_matches(seed, upper, page, u, v)) {
                return seed.atlasClass;
            }
        }
        return LiquidAtlasClass::None;
    }

    LiquidAtlasClass classify_liquid_atlas_key(const TileInfo &tileInfo) noexcept {
        (void) tileInfo;
        // The current TileInfo surface does not carry a tileset resref.
        // Callers that know the tileset should use the explicit overload.
        return LiquidAtlasClass::None;
    }

    bool LiquidMaskGrid::any() const noexcept {
        return std::any_of(cells.begin(), cells.end(), [](std::uint8_t value) { return value != 0; });
    }

    std::size_t LiquidMaskGrid::count() const noexcept {
        return static_cast<std::size_t>(std::count_if(cells.begin(), cells.end(),
                                                      [](std::uint8_t value) { return value != 0; }));
    }

    bool LiquidMaskGrid::at(int column, int row) const noexcept {
        if (column < 0 || row < 0 || column >= columns || row >= rows) {
            return false;
        }

        const auto index = static_cast<std::size_t>(row * columns + column);
        return index < cells.size() && cells[index] != 0;
    }

    bool liquid_color_matches(TileLiquidMode mode,
                              std::uint8_t r,
                              std::uint8_t g,
                              std::uint8_t b,
                              std::uint8_t a) noexcept {
        switch (mode) {
            case TileLiquidMode::Water:
                return water_like(r, g, b, a);
            case TileLiquidMode::Lava:
                return lava_like(r, g, b, a);
            case TileLiquidMode::Goo:
                return goo_like(r, g, b, a);
            case TileLiquidMode::Sewage:
                return sewage_like(r, g, b, a);
            case TileLiquidMode::Swamp:
                return swamp_like(r, g, b, a);
            case TileLiquidMode::None:
            default:
                return false;
        }
    }

    LiquidMaskGrid build_liquid_mask_grid(TileLiquidMode mode,
                                          const std::uint8_t *rgba,
                                          int textureWidth,
                                          int textureHeight,
                                          int u0,
                                          int v0,
                                          int du,
                                          int dv,
                                          int columns,
                                          int rows) noexcept {
        LiquidMaskGrid mask{};
        mask.columns = std::max(columns, 1);
        mask.rows = std::max(rows, 1);
        mask.cells.assign(static_cast<std::size_t>(mask.columns * mask.rows), 0);

        if (!rgba || mode == TileLiquidMode::None || textureWidth <= 0 || textureHeight <= 0 || du <= 0 || dv <= 0) {
            return mask;
        }

        const int x0 = std::clamp(u0, 0, textureWidth);
        const int y0 = std::clamp(v0, 0, textureHeight);
        const int x1 = std::clamp(u0 + du, 0, textureWidth);
        const int y1 = std::clamp(v0 + dv, 0, textureHeight);
        if (x1 <= x0 || y1 <= y0) {
            return mask;
        }

        std::vector<float> keyedRatios(mask.cells.size(), 0.0f);

        for (int row = 0; row < mask.rows; ++row) {
            const int sampleY0 = y0 + ((y1 - y0) * row) / mask.rows;
            const int sampleY1 = y0 + ((y1 - y0) * (row + 1)) / mask.rows;
            for (int column = 0; column < mask.columns; ++column) {
                const int sampleX0 = x0 + ((x1 - x0) * column) / mask.columns;
                const int sampleX1 = x0 + ((x1 - x0) * (column + 1)) / mask.columns;

                std::size_t opaqueCount = 0;
                std::size_t keyedCount = 0;
                std::uint64_t sumR = 0;
                std::uint64_t sumG = 0;
                std::uint64_t sumB = 0;
                std::uint64_t sumA = 0;
                for (int sampleY = sampleY0; sampleY < sampleY1; ++sampleY) {
                    for (int sampleX = sampleX0; sampleX < sampleX1; ++sampleX) {
                        const auto index = static_cast<std::size_t>((sampleY * textureWidth + sampleX) * 4);
                        const auto r = rgba[index + 0];
                        const auto g = rgba[index + 1];
                        const auto b = rgba[index + 2];
                        const auto a = rgba[index + 3];
                        if (a < 16) continue;
                        ++opaqueCount;
                        sumR += r;
                        sumG += g;
                        sumB += b;
                        sumA += a;
                        if (liquid_color_matches(mode, r, g, b, a)) {
                            ++keyedCount;
                        }
                    }
                }

                const auto index = static_cast<std::size_t>(row * mask.columns + column);
                if (opaqueCount == 0) {
                    continue;
                }

                keyedRatios[index] = static_cast<float>(keyedCount) / static_cast<float>(opaqueCount);
                const auto avgR = static_cast<std::uint8_t>(sumR / opaqueCount);
                const auto avgG = static_cast<std::uint8_t>(sumG / opaqueCount);
                const auto avgB = static_cast<std::uint8_t>(sumB / opaqueCount);
                const auto avgA = static_cast<std::uint8_t>(sumA / opaqueCount);
                const bool averageMatches = liquid_color_matches(mode, avgR, avgG, avgB, avgA);
                if (averageMatches || keyedCount * 12 >= opaqueCount || keyedCount >= 2) {
                    mask.cells[index] = 1;
                }
            }
        }

        const auto original = mask.cells;
        for (int row = 0; row < mask.rows; ++row) {
            for (int column = 0; column < mask.columns; ++column) {
                const auto index = static_cast<std::size_t>(row * mask.columns + column);
                if (original[index] != 0 || keyedRatios[index] < 0.02f) {
                    continue;
                }

                int neighbors = 0;
                neighbors += (column > 0 && original[index - 1] != 0) ? 1 : 0;
                neighbors += (column + 1 < mask.columns && original[index + 1] != 0) ? 1 : 0;
                neighbors += (row > 0 && original[index - mask.columns] != 0) ? 1 : 0;
                neighbors += (row + 1 < mask.rows && original[index + mask.columns] != 0) ? 1 : 0;
                if (neighbors >= 1) {
                    mask.cells[index] = 1;
                }
            }
        }

        const auto expanded = mask.cells;
        for (int row = 0; row < mask.rows; ++row) {
            for (int column = 0; column < mask.columns; ++column) {
                const auto index = static_cast<std::size_t>(row * mask.columns + column);
                if (expanded[index] != 0 || keyedRatios[index] < 0.005f) {
                    continue;
                }

                int neighbors = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = column + dx;
                        const int ny = row + dy;
                        if (nx < 0 || ny < 0 || nx >= mask.columns || ny >= mask.rows) continue;
                        if (expanded[static_cast<std::size_t>(ny * mask.columns + nx)] != 0) {
                            ++neighbors;
                        }
                    }
                }
                if (neighbors >= 2) {
                    mask.cells[index] = 1;
                }
            }
        }

        prune_small_components(mask);
        return mask;
    }

    LiquidMaskGrid build_liquid_atlas_mask_grid(LiquidAtlasClass atlasClass,
                                                int columns,
                                                int rows) noexcept {
        switch (atlasClass) {
            case LiquidAtlasClass::OpenWater:
                return make_full_mask(columns, rows);
            case LiquidAtlasClass::BasinWater:
                return make_basin_mask(columns, rows);
            case LiquidAtlasClass::None:
            default:
                return make_empty_mask(columns, rows);
        }
    }

    LiquidBaseReplacementPath select_liquid_base_replacement(bool wedCovered,
                                                             LiquidAtlasClass atlasClass,
                                                             bool heuristicMaskAny) noexcept {
        if (!wedCovered) {
            return LiquidBaseReplacementPath::None;
        }
        if (atlasClass != LiquidAtlasClass::None) {
            return LiquidBaseReplacementPath::AtlasKeyed;
        }
        if (heuristicMaskAny) {
            return LiquidBaseReplacementPath::Heuristic;
        }
        return LiquidBaseReplacementPath::None;
    }
}
