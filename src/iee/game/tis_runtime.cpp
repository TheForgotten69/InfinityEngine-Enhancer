#include "tis_runtime.h"

#include "iee/core/pattern_scanner.h"

namespace iee::game {
    bool get_tile_info(void *vidTile,
                       const BuildManifest &manifest,
                       TileInfo &out,
                       void *(*CRes_Demand)(void *)) {
        out = {};

        if (!vidTile) {
            return false;
        }

        const auto readAddr = reinterpret_cast<std::uintptr_t>(vidTile) + manifest.offsets.vidTileResource;
        if (readAddr < 0x1000) {
            return false;
        }

        CResTile *resource = nullptr;
        if (!core::safe_read(reinterpret_cast<const void *>(readAddr), resource) || !resource) {
            return false;
        }

        CResTile resourceSnapshot;
        if (!core::safe_read(resource, resourceSnapshot) || !resourceSnapshot.tis) {
            return false;
        }

        if (CRes_Demand) {
            try {
                (void) CRes_Demand(resourceSnapshot.tis);
            } catch (...) {
                return false;
            }
        }

        CResTileSet tilesetSnapshot;
        if (!core::safe_read(resourceSnapshot.tis, tilesetSnapshot)) {
            return false;
        }

        if (!tilesetSnapshot.baseclass_0.pData || tilesetSnapshot.baseclass_0.nSize == 0 || resourceSnapshot.tileIndex < 0) {
            return false;
        }

        const auto *table = static_cast<const PVRZTileEntry *>(tilesetSnapshot.baseclass_0.pData);
        PVRZTileEntry entry{};
        if (!core::safe_read(table + resourceSnapshot.tileIndex, entry)) {
            return false;
        }

        out.resource = resource;
        out.tileset = resourceSnapshot.tis;
        out.table = table;
        out.header = tilesetSnapshot.h;
        out.index = resourceSnapshot.tileIndex;
        out.tileDataBlockLen = tilesetSnapshot.baseclass_0.nSize;
        out.runtimeTileDimension = tilesetSnapshot.baseclass_0.nCount;

        return true;
    }

    bool get_tis_linear_tiles_flag(const CResTileSet *tis, const BuildManifest &manifest) {
        if (!tis) {
            return false;
        }

        const auto *flagAddr = reinterpret_cast<const std::byte *>(tis) + manifest.offsets.tisLinearTilesFlag;
        int flag = 0;
        return core::safe_read(flagAddr, flag) && flag != 0;
    }

    std::optional<std::uint32_t> get_tis_header_tile_dimension(const TileInfo &tileInfo,
                                                               const BuildManifest &manifest) {
        if (!tileInfo.header) {
            return std::nullopt;
        }

        std::uint32_t tileDimension = 0;
        const auto *fieldAddr = reinterpret_cast<const std::byte *>(tileInfo.header) +
                                manifest.offsets.tisHeaderTileDimension;
        if (!core::safe_read(fieldAddr, tileDimension)) {
            return std::nullopt;
        }

        return tileDimension;
    }
}
