#include "wed_runtime.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace iee::game {
    namespace {
        constexpr std::uint32_t kWedFileType = 0x20444557;     // "WED "
        constexpr std::uint32_t kWedFileVersion = 0x332E3156;  // "V1.3"

        template<typename T>
        bool read_struct(const std::byte *base, std::size_t size, std::size_t offset, T &out) noexcept {
            if (offset > size || size - offset < sizeof(T)) {
                return false;
            }

            std::memcpy(&out, base + offset, sizeof(T));
            return true;
        }

        ResrefBuffer copy_resref(const char (&raw)[8]) noexcept {
            ResrefBuffer out{};
            out.fill('\0');
            for (std::size_t i = 0; i < 8; ++i) {
                const auto c = raw[i];
                if (c == '\0') break;
                out[i] = c;
            }
            return out;
        }

        ResrefBuffer copy_resref(const std::array<std::uint8_t, 8> &raw) noexcept {
            ResrefBuffer out{};
            out.fill('\0');
            for (std::size_t i = 0; i < 8; ++i) {
                const auto c = static_cast<char>(raw[i]);
                if (c == '\0') break;
                out[i] = c;
            }
            return out;
        }
    }

    std::string_view WedOverlayInfo::tilesetResrefView() const noexcept {
        return resref_view(tilesetResref);
    }

    std::string_view WedAreaInfo::areaResrefView() const noexcept {
        return resref_view(areaResref);
    }

    bool parse_loaded_wed(const CRes &resource, WedAreaInfo &out) noexcept {
        out = {};

        if (!resource.bLoaded || !resource.pData || resource.nSize < sizeof(WED_WedHeader_st)) {
            return false;
        }

        const auto *base = static_cast<const std::byte *>(resource.pData);
        const auto size = static_cast<std::size_t>(resource.nSize);

        WED_WedHeader_st header{};
        if (!read_struct(base, size, 0, header)) {
            return false;
        }

        if (header.nFileType != kWedFileType || header.nFileVersion != kWedFileVersion) {
            return false;
        }

        if (resource.resref) {
            (void) read_runtime_resref(resource.resref, out.areaResref);
        }

        out.overlayCount = header.nLayers;
        out.overlays.reserve(header.nLayers);

        for (std::uint32_t i = 0; i < header.nLayers; ++i) {
            WED_LayerHeader_st entry{};
            const auto overlayOffset = static_cast<std::size_t>(header.nOffsetToLayerHeaders) + i * sizeof(WED_LayerHeader_st);
            if (!read_struct(base, size, overlayOffset, entry)) {
                out = {};
                return false;
            }

            WedOverlayInfo overlay{};
            overlay.width = entry.nTilesAcross;
            overlay.height = entry.nTilesDown;
            overlay.tilesetResref = copy_resref(entry.rrTileSet);
            overlay.uniqueTileCount = entry.nNumUniqueTiles;
            overlay.movementType = entry.nLayerFlags;
            overlay.tilemapOffset = entry.nOffsetToTileData;
            overlay.tileIndexLookupOffset = entry.nOffsetToTileList;
            overlay.liquidMode = classify_liquid_tileset(overlay.tilesetResrefView());
            out.overlays.push_back(std::move(overlay));
        }

        if (out.overlays.empty()) {
            return true;
        }

        out.baseWidth = out.overlays.front().width;
        out.baseHeight = out.overlays.front().height;

        const auto cellCount = static_cast<std::size_t>(out.baseWidth) * static_cast<std::size_t>(out.baseHeight);
        if (cellCount == 0) {
            return true;
        }

        out.baseOverlayFlags.resize(cellCount, 0);
        const auto baseTilemapOffset = static_cast<std::size_t>(out.overlays.front().tilemapOffset);
        for (std::size_t cell = 0; cell < cellCount; ++cell) {
            WED_TileData_st tilemap{};
            const auto tilemapOffset = baseTilemapOffset + cell * sizeof(WED_TileData_st);
            if (!read_struct(base, size, tilemapOffset, tilemap)) {
                out.baseOverlayFlags.clear();
                return true;
            }

            out.baseOverlayFlags[cell] = tilemap.bFlags;
            for (std::size_t overlayIndex = 1; overlayIndex < out.overlays.size() && overlayIndex <= 7; ++overlayIndex) {
                const auto mask = static_cast<std::uint8_t>(1u << overlayIndex);
                if ((tilemap.bFlags & mask) != 0) {
                    ++out.overlays[overlayIndex].coverageCells;
                }
            }
        }

        return true;
    }

    std::uint8_t liquid_overlay_mask(const WedAreaInfo &wed) noexcept {
        std::uint8_t mask = 0;
        for (std::size_t overlayIndex = 1; overlayIndex < wed.overlays.size() && overlayIndex <= 7; ++overlayIndex) {
            if (wed.overlays[overlayIndex].liquidMode != TileLiquidMode::None) {
                mask = static_cast<std::uint8_t>(mask | static_cast<std::uint8_t>(1u << overlayIndex));
            }
        }
        return mask;
    }

    bool base_cell_has_liquid_overlay(const WedAreaInfo &wed,
                                      std::size_t cellIndex,
                                      std::uint8_t &outOverlayFlags) noexcept {
        outOverlayFlags = 0;
        if (cellIndex >= wed.baseOverlayFlags.size()) {
            return false;
        }

        outOverlayFlags = wed.baseOverlayFlags[cellIndex];
        const auto mask = liquid_overlay_mask(wed);
        return (outOverlayFlags & mask) != 0;
    }

    std::optional<std::size_t> base_cell_index_from_screen_point(const WedAreaInfo &wed,
                                                                 int screenX,
                                                                 int screenY,
                                                                 int worldOffsetX,
                                                                 int worldOffsetY) noexcept {
        if (wed.baseWidth == 0 || wed.baseHeight == 0) {
            return std::nullopt;
        }

        const auto worldX = static_cast<long long>(screenX) + static_cast<long long>(worldOffsetX);
        const auto worldY = static_cast<long long>(screenY) + static_cast<long long>(worldOffsetY);
        if (worldX < 0 || worldY < 0) {
            return std::nullopt;
        }

        const auto column = static_cast<std::size_t>(worldX / TileDimensions::RENDER_QUAD_SIZE);
        const auto row = static_cast<std::size_t>(worldY / TileDimensions::RENDER_QUAD_SIZE);
        if (column >= wed.baseWidth || row >= wed.baseHeight) {
            return std::nullopt;
        }

        return row * static_cast<std::size_t>(wed.baseWidth) + column;
    }
}
