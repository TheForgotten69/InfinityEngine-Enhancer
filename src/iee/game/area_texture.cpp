#include "area_texture.h"
#include "tile_liquid.h"

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

    std::optional<AreaCellTexture> pack_area_liquid_texture(const WedAreaInfo &wed) {
        const std::size_t expected = std::size_t{wed.baseWidth} * wed.baseHeight;
        if (expected == 0 || wed.baseOverlayFlags.size() != expected) {
            return std::nullopt;
        }
        AreaCellTexture out{.width = wed.baseWidth, .height = wed.baseHeight};
        out.texels.resize(expected, 0);
        for (std::size_t cell = 0; cell < expected; ++cell) {
            const std::uint8_t flags = wed.baseOverlayFlags[cell];
            for (std::size_t overlay = 1; overlay < wed.overlays.size() && overlay <= 7; ++overlay) {
                if ((flags & (1u << overlay)) == 0) continue;
                const auto mode = wed.overlays[overlay].liquidMode;
                if (mode != TileLiquidMode::None) {
                    out.texels[cell] = static_cast<std::uint8_t>(mode);
                    break; // lowest overlay index wins
                }
            }
        }
        return out;
    }

    std::optional<AreaCellTexture> build_fine_liquid_mask(const WedAreaInfo &wed,
                                                          const OverlayTileAlphaProvider &tileAlpha) {
        const std::size_t cellCount = std::size_t{wed.baseWidth} * wed.baseHeight;
        if (cellCount == 0 || wed.baseOverlayFlags.size() != cellCount) {
            return std::nullopt;
        }

        constexpr int kBlock = kTilePixels / kFineMaskTexelsPerCell; // 8x8 pixels per texel
        AreaCellTexture out{.width = wed.baseWidth * kFineMaskTexelsPerCell,
                            .height = wed.baseHeight * kFineMaskTexelsPerCell};
        out.texels.resize(std::size_t{static_cast<std::size_t>(out.width)} * out.height, 0);

        for (std::size_t cell = 0; cell < cellCount; ++cell) {
            const std::uint8_t flags = wed.baseOverlayFlags[cell];
            std::size_t overlayIndex = 0;
            TileLiquidMode mode = TileLiquidMode::None;
            for (std::size_t overlay = 1; overlay < wed.overlays.size() && overlay <= 7; ++overlay) {
                if ((flags & (1u << overlay)) == 0) continue;
                if (wed.overlays[overlay].liquidMode != TileLiquidMode::None) {
                    overlayIndex = overlay;
                    mode = wed.overlays[overlay].liquidMode;
                    break; // lowest overlay index wins
                }
            }
            if (mode == TileLiquidMode::None) {
                continue;
            }

            // Overlay tilemaps use the same cell coordinates as the base grid.
            const auto &overlay = wed.overlays[overlayIndex];
            const auto cellX = cell % wed.baseWidth;
            const auto cellY = cell / wed.baseWidth;
            std::optional<TileAlpha> alpha;
            if (!overlay.cellTileIndex.empty() && cellX < overlay.width && cellY < overlay.height) {
                const auto tileIndex = overlay.cellTileIndex[cellY * overlay.width + cellX];
                if (tileIndex != 0xFFFF && tileAlpha) {
                    alpha = tileAlpha(overlayIndex, tileIndex);
                }
            }

            const auto modeTexel = static_cast<std::uint8_t>(mode);
            for (int by = 0; by < kFineMaskTexelsPerCell; ++by) {
                for (int bx = 0; bx < kFineMaskTexelsPerCell; ++bx) {
                    std::uint8_t value = modeTexel; // full-cell fallback
                    if (alpha) {
                        int opaqueCount = 0;
                        for (int py = 0; py < kBlock; ++py) {
                            for (int px = 0; px < kBlock; ++px) {
                                opaqueCount += alpha->opaque[(by * kBlock + py) * kTilePixels +
                                                             bx * kBlock + px];
                            }
                        }
                        value = (opaqueCount * 2 >= kBlock * kBlock) ? modeTexel : 0;
                    }
                    const auto texelX = cellX * kFineMaskTexelsPerCell + bx;
                    const auto texelY = cellY * kFineMaskTexelsPerCell + by;
                    out.texels[texelY * static_cast<std::size_t>(out.width) + texelX] = value;
                }
            }
        }
        return out;
    }
}
