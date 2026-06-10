#pragma once

#include <array>
#include <cstdint>

namespace iee::game {
    struct CInfTileSet;

#pragma pack(push, 1)
    struct PVRTextureHeaderV3 {
        std::uint32_t u32Version{};
        std::uint32_t u32Flags{};
        std::uint32_t u64PixelFormatlo{};
        std::uint32_t u64PixelFormathi{};
        std::uint32_t u32ColourSpace{};
        std::uint32_t u32ChannelType{};
        std::uint32_t u32Height{};
        std::uint32_t u32Width{};
        std::uint32_t u32Depth{};
        std::uint32_t u32NumSurfaces{};
        std::uint32_t u32NumFaces{};
        std::uint32_t u32MIPMapCount{};
        std::uint32_t u32MetaDataSize{};
    };

    struct ResFixedHeader_st {
        std::uint32_t nFileType{};
        std::uint32_t nFileVersion{};
        std::uint32_t nNumber{};
        std::uint32_t nSize{};
        std::uint32_t nTableOffset{};
    };

    struct TisFileHeader {
        std::uint32_t nFileType{};
        std::uint32_t nFileVersion{};
        std::uint32_t nNumber{};
        std::uint32_t nSize{};
        std::uint32_t nTableOffset{};
        std::uint32_t tileDimension{};
    };

    struct WED_LayerHeader_st {
        std::uint16_t nTilesAcross{};
        std::uint16_t nTilesDown{};
        std::array<std::uint8_t, 8> rrTileSet{};
        std::uint16_t nNumUniqueTiles{};
        std::uint16_t nLayerFlags{};
        std::uint32_t nOffsetToTileData{};
        std::uint32_t nOffsetToTileList{};
    };

    struct WED_PolyHeader_st {
        std::uint32_t nPolys{};
        std::uint32_t nOffsetToPolyList{};
        std::uint32_t nOffsetToPointList{};
        std::uint32_t nOffsetToScreenSectionList{};
        std::uint32_t nOffsetToScreenPolyList{};
    };

    struct WED_PolyList_st {
        std::uint32_t nStartingPoint{};
        std::uint32_t nNumPoints{};
        std::uint8_t nType{};
        std::uint8_t nHeight{};
        std::uint16_t nLeft{};
        std::uint16_t nRight{};
        std::uint16_t nTop{};
        std::uint16_t nBottom{};
    };

    struct WED_PolyPoint_st {
        std::uint16_t x{};
        std::uint16_t y{};
    };

    struct WED_ScreenSectionList {
        std::uint16_t nStartingPoly{};
        std::uint16_t nNumPolys{};
    };

    struct WED_TileData_st {
        std::uint16_t nStartingTile{};
        std::uint16_t nNumTiles{};
        std::int16_t nSecondary{};
        std::uint8_t bFlags{};
        std::uint8_t bAnimSpeed{};
        std::uint16_t wFlags{};
    };

    struct WED_TiledObject_st {
        std::array<std::uint8_t, 8> resID{};
        std::uint16_t bType{};
        std::uint16_t nStartingTile{};
        std::uint16_t nNumTiles{};
        std::uint16_t nNumPrimaryPolys{};
        std::uint16_t nNumSecondaryPolys{};
        std::uint32_t nOffsetToPrimaryPolys{};
        std::uint32_t nOffsetToSecondaryPolys{};
    };

    struct WED_WedHeader_st {
        std::uint32_t nFileType{};
        std::uint32_t nFileVersion{};
        std::uint32_t nLayers{};
        std::uint32_t nTiledObjects{};
        std::uint32_t nOffsetToLayerHeaders{};
        std::uint32_t nOffsetToPolyHeader{};
        std::uint32_t nOffsetToTiledObjects{};
        std::uint32_t nOffsetToObjectTileList{};
        std::uint16_t nVisiblityRange{};
        std::uint16_t nChanceOfRain{};
        std::uint16_t nChanceOfFog{};
        std::uint16_t nChanceOfSnow{};
        std::uint32_t dwFlags{};
    };

    struct bamHeader_st {
        std::uint32_t nFileType{};
        std::uint32_t nFileVersion{};
        std::uint16_t nFrames{};
        std::uint8_t nSequences{};
        std::uint8_t nTransparentColor{};
        std::uint32_t nTableOffset{};
        std::uint32_t nPaletteOffset{};
        std::uint32_t nFrameListOffset{};
    };

    struct BAMHEADERV2 {
        std::uint32_t nFileType{};
        std::uint32_t nFileVersion{};
        std::uint32_t nFrames{};
        std::uint32_t nSequences{};
        std::uint32_t nQuads{};
        std::uint32_t nFramesOffset{};
        std::uint32_t nSequencesOffset{};
        std::uint32_t nQuadsOffset{};
    };

    struct frame {
        std::uint8_t *data{};
        std::uint64_t length{};
        frame *next{};
    };

    struct frameTableEntry_st {
        std::uint16_t nWidth{};
        std::uint16_t nHeight{};
        std::int16_t nCenterX{};
        std::int16_t nCenterY{};
        std::uint32_t ___u4{};
    };

    struct st_tiledef {
        std::int32_t nTile{};
        std::int32_t nUsageCount{};
        std::int32_t texture{};
        std::uint32_t _pad0{};
        CInfTileSet *pTileSet{};
    };
#pragma pack(pop)

    static_assert(sizeof(PVRTextureHeaderV3) == 0x34);
    static_assert(sizeof(ResFixedHeader_st) == 0x14);
    static_assert(sizeof(TisFileHeader) == 0x18);
    static_assert(sizeof(WED_LayerHeader_st) == 0x18);
    static_assert(sizeof(WED_PolyHeader_st) == 0x14);
    static_assert(sizeof(WED_PolyList_st) == 0x12);
    static_assert(sizeof(WED_PolyPoint_st) == 0x4);
    static_assert(sizeof(WED_ScreenSectionList) == 0x4);
    static_assert(sizeof(WED_TileData_st) == 0xA);
    static_assert(sizeof(WED_TiledObject_st) == 0x1A);
    static_assert(sizeof(WED_WedHeader_st) == 0x2C);
    static_assert(sizeof(bamHeader_st) == 0x18);
    static_assert(sizeof(BAMHEADERV2) == 0x20);
    static_assert(sizeof(frame) == 0x18);
    static_assert(sizeof(frameTableEntry_st) == 0xC);
    static_assert(sizeof(st_tiledef) == 0x18);
}
