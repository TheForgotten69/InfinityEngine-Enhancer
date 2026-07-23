#pragma once

#include <array>
#include <cstddef>
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

    // ARE V1.0 (BGEE). Only the fields this project consumes are modeled:
    // the fixed header prefix that carries the animation section pointers and
    // the 76-byte ambient-animation record (layout verified against
    // NearInfinity's org.infinity.resource.are.{AreResource,Animation}).
    struct ARE_Header_st {
        std::uint32_t nFileType{};      // "AREA"
        std::uint32_t nFileVersion{};   // "V1.0"
        std::array<std::uint8_t, 8> rrWed{};
        std::uint32_t nLastSaved{};
        std::uint32_t nAreaFlags{};
        std::array<std::uint8_t, 48> edges{};        // 4 x (resref + edge flags)
        std::uint16_t nLocationFlags{};              // +0x48
        std::uint16_t nRainProbability{};
        std::uint16_t nSnowProbability{};
        std::uint16_t nFogProbability{};
        std::uint16_t nLightningProbability{};
        std::uint8_t nOverlayTransparency{};         // EE; classic wind speed low byte
        std::uint8_t ___u0{};
        std::uint32_t nActorsOffset{};               // +0x54
        std::uint16_t nActors{};
        std::uint16_t nTriggers{};
        std::uint32_t nTriggersOffset{};
        std::uint32_t nSpawnPointsOffset{};
        std::uint32_t nSpawnPoints{};
        std::uint32_t nEntrancesOffset{};
        std::uint32_t nEntrances{};
        std::uint32_t nContainersOffset{};
        std::uint16_t nContainers{};
        std::uint16_t nItems{};
        std::uint32_t nItemsOffset{};
        std::uint32_t nVerticesOffset{};
        std::uint16_t nVertices{};
        std::uint16_t nAmbients{};
        std::uint32_t nAmbientsOffset{};
        std::uint32_t nVariablesOffset{};
        std::uint16_t nVariables{};
        std::uint16_t nObjectFlags{};
        std::uint32_t nObjectFlagsOffset{};
        std::array<std::uint8_t, 8> rrAreaScript{};
        std::uint32_t nExploredBitmapSize{};
        std::uint32_t nExploredBitmapOffset{};
        std::uint32_t nDoors{};
        std::uint32_t nDoorsOffset{};
        std::uint32_t nAnimations{};                 // +0xAC
        std::uint32_t nAnimationsOffset{};           // +0xB0
    };

    struct ARE_Animation_st {
        std::array<std::uint8_t, 32> szName{};
        std::uint16_t nX{};
        std::uint16_t nY{};
        std::uint32_t nSchedule{};                   // hour-of-day bitmask
        std::array<std::uint8_t, 8> rrAnimation{};   // BAM; EE also WBM/PVRZ
        std::uint16_t nAnimationIndex{};
        std::uint16_t nFrameIndex{};
        std::uint32_t nFlags{};
        std::int16_t nHeight{};
        std::uint16_t nTranslucency{};
        std::uint16_t nStartRange{};
        std::uint8_t nLoopProbability{};
        std::uint8_t nStartDelay{};
        std::array<std::uint8_t, 8> rrPalette{};
        std::uint16_t nMovieWidth{};                 // EE
        std::uint16_t nMovieHeight{};                // EE
    };

    // ARE_Animation_st.nFlags bits (NearInfinity FLAGS_ARRAY, EE variant).
    inline constexpr std::uint32_t kAreAnimationFlagIsShown = 1u << 0;
    inline constexpr std::uint32_t kAreAnimationFlagNoShadow = 1u << 1;
    inline constexpr std::uint32_t kAreAnimationFlagNotLightSource = 1u << 2;
    inline constexpr std::uint32_t kAreAnimationFlagDrawAsBackground = 1u << 8;
    inline constexpr std::uint32_t kAreAnimationFlagUseWbm = 1u << 13;
    inline constexpr std::uint32_t kAreAnimationFlagUsePvrz = 1u << 15;

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
    static_assert(sizeof(ARE_Header_st) == 0xB4);
    static_assert(offsetof(ARE_Header_st, nAnimations) == 0xAC);
    static_assert(offsetof(ARE_Header_st, nAnimationsOffset) == 0xB0);
    static_assert(sizeof(ARE_Animation_st) == 0x4C);
    static_assert(offsetof(ARE_Animation_st, rrAnimation) == 0x28);
    static_assert(offsetof(ARE_Animation_st, nFlags) == 0x34);
    static_assert(sizeof(bamHeader_st) == 0x18);
    static_assert(sizeof(BAMHEADERV2) == 0x20);
    static_assert(sizeof(frame) == 0x18);
    static_assert(sizeof(frameTableEntry_st) == 0xC);
    static_assert(sizeof(st_tiledef) == 0x18);
}
