#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "build_manifest.h"

namespace iee::game {
    struct view_t {
        std::byte data[24]{};
    };

    struct CRes {
        void *vfptr{};
        const char *resref{};
        int32_t type{};
        int32_t _pad0{};
        view_t view{};
        uint32_t nID{};
        int32_t zip_id{};
        int32_t override_id{};
        int32_t _pad1{};
        void *pData{};
        uint32_t nSize{};
        uint32_t nCount{};
        bool bWasMalloced{};
        bool bLoaded{};
        std::byte _pad2[6]{};
    };

    struct CSize {
        int32_t cx{};
        int32_t cy{};
    };

    struct TisFileHeader {
        uint32_t nFileType{};
        uint32_t nFileVersion{};
        uint32_t nNumber{};
        uint32_t nSize{};
        uint32_t nTableOffset{};
        uint32_t tileDimension{};
    };

    struct CResPVR {
        CRes baseclass_0{};
        int32_t texture{};
        int32_t format{};
        int32_t filtering{};
        CSize size{};
        int32_t _pad0{};
    };

    struct CResTileSet {
        CRes baseclass_0{};
        TisFileHeader *h{};
    };

    struct CResRef {
        std::array<char, 8> m_resRef{};
    };

    struct PVRZTileEntry {
        int32_t page{};
        int32_t u{};
        int32_t v{};
    };

    struct CResTile {
        CResTileSet *tis{};
        int32_t tileIndex{};
        int32_t _pad0{};
        CResPVR *pvr{};
    };

    struct TileInfo {
        const CResTile *resource{};
        const CResTileSet *tileset{};
        const PVRZTileEntry *table{};
        const TisFileHeader *header{};
        int index{-1};
        uint32_t tileDataBlockLen{};
        uint32_t runtimeTileDimension{};
    };

    [[nodiscard]] bool get_tile_info(void *vidTile,
                                     const BuildManifest &manifest,
                                     TileInfo &out,
                                     void *(*CRes_Demand)(void *));

    [[nodiscard]] bool get_tis_linear_tiles_flag(const CResTileSet *tis, const BuildManifest &manifest);

    [[nodiscard]] std::optional<std::uint32_t> get_tis_header_tile_dimension(const TileInfo &tileInfo,
                                                                             const BuildManifest &manifest);
}
