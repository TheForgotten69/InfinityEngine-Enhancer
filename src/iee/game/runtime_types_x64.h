#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "file_formats.h"

namespace iee::game {
    // Curated x64 runtime layouts for render-adjacent reverse engineering.
    // Small PODs are modeled directly. Large engine objects keep exact offsets,
    // but leave most interior ranges opaque unless they already pay for themselves.

    struct CGameArea;
    struct CInfGame;

    struct view_t {
        std::byte data[24]{};
    };

    struct tagPOINT {
        std::int32_t x{};
        std::int32_t y{};
    };

    using CPoint = tagPOINT;

    struct tagSIZE {
        std::int32_t cx{};
        std::int32_t cy{};
    };

    using CSize = tagSIZE;

    struct tagRECT {
        std::int32_t left{};
        std::int32_t top{};
        std::int32_t right{};
        std::int32_t bottom{};
    };

    using CRect = tagRECT;

    struct tagRGBQUAD {
        std::uint8_t rgbBlue{};
        std::uint8_t rgbGreen{};
        std::uint8_t rgbRed{};
        std::uint8_t rgbReserved{};
    };

    struct CRes {
        void *vfptr{};
        const char *resref{};
        std::int32_t type{};
        std::int32_t _pad0{};
        view_t view{};
        std::uint32_t nID{};
        std::int32_t zip_id{};
        std::int32_t override_id{};
        std::int32_t _pad1{};
        void *pData{};
        std::uint32_t nSize{};
        std::uint32_t nCount{};
        bool bWasMalloced{};
        bool bLoaded{};
        std::byte _pad2[6]{};
    };

    struct CResRef {
        std::array<char, 8> m_resRef{};
    };

    struct CResPVR {
        CRes baseclass_0{};
        std::int32_t texture{};
        std::int32_t format{};
        std::int32_t filtering{};
        CSize size{};
        std::int32_t _pad0{};
    };

    struct CResTileSet {
        CRes baseclass_0{};
        TisFileHeader *h{};
    };

    struct CResTile {
        CResTileSet *tis{};
        std::int32_t tileIndex{};
        std::int32_t _pad0{};
        CResPVR *pvr{};
    };

    struct CResWED {
        CRes baseclass_0{};
        void *pWEDHeader{};
        void *pLayers{};
        void *pPolyHeader{};
        void *pScreenSectionList{};
        void *pPolyList{};
        void *pPolyPoints{};
    };

    struct CVidPalette {
        std::uint64_t m_nAUCounter{};
        std::uint64_t m_nAUCounterBase{};
        tagRGBQUAD *m_pPalette{};
        std::int32_t m_nEntries{};
        std::uint32_t rgbGlobalTint{};
        std::uint16_t m_nType{};
        std::uint8_t m_bPaletteOwner{};
        std::byte _pad0[1]{};
        std::int32_t m_bSubRangesCalculated{};
        std::array<std::uint8_t, 7> m_rangeColors{};
        std::byte _pad1[1]{};
    };

    struct CVIDIMG_PALETTEAFFECT {
        std::uint32_t rgbTintColor{};
        std::uint32_t rgbAddColor{};
        std::uint32_t rgbLightColor{};
        std::byte _pad0[4]{};
        std::array<std::uint32_t *, 7> pRangeTints{};
        std::array<std::uint8_t, 8> aRangeTintPeriods{};
        std::array<std::uint32_t *, 7> pRangeAdds{};
        std::array<std::uint8_t, 8> aRangeAddPeriods{};
        std::array<std::uint32_t *, 7> pRangeLights{};
        std::array<std::uint8_t, 7> aRangeLightPeriods{};
        std::uint8_t suppressTints{};
    };

    struct CVidImage {
        CVidPalette m_cPalette{};
        CVIDIMG_PALETTEAFFECT mPaletteAffects{};
    };

    struct CVidTile {
        CVidImage baseclass_0{};
        CResTile *pRes{};
        std::uint32_t m_dwFlags{};
        std::byte _pad0[4]{};
    };

    struct CVidCell {
        void *vfptr{};
        CVidImage baseclass_0{};
        std::array<std::byte, 16> baseclass_1{};
        std::int16_t m_nCurrentFrame{};
        std::uint16_t m_nCurrentSequence{};
        std::int32_t m_nAnimType{};
        std::int32_t m_bPaletteChanged{};
        std::byte _pad0[4]{};
        void *m_pFrame{};
        std::uint8_t m_bShadowOn{};
        std::byte _pad1[7]{};
    };

    struct CVidBitmapOpaque {
        std::array<std::byte, 288> data{};
    };

    struct CVidMode {
        std::int32_t m_nPrintFile{};
        std::int32_t m_nPointerNumber{};
        std::uint32_t m_dwCursorRenderFlags{};
        std::uint32_t m_dwRedMask{};
        std::uint32_t m_dwGreenMask{};
        std::uint32_t m_dwBlueMask{};
        std::uint8_t m_bFadeTo{};
        std::uint8_t m_nFade{};
        std::byte _pad0[6]{};
        void *m_pWindow{};
        void *m_glContext{};
        CVidBitmapOpaque m_circle{};
        std::int32_t nWidth{};
        std::int32_t nHeight{};
        bool bRedrawEntireScreen{};
        bool bHardwareMouseCursor{};
        std::byte _pad1[6]{};
        CVidCell *pPointerVidCell{};
        CVidCell *pTooltipVidCell{};
        std::uint8_t m_bPrintScreen{};
        std::byte _pad2[3]{};
        std::uint32_t nTickCount{};
        float m_fInputScale{};
        std::uint32_t rgbGlobalTint{};
        std::uint8_t m_nGammaCorrection{};
        std::uint8_t m_nBrightnessCorrection{};
        std::byte _pad3[2]{};
        std::int32_t m_nScreenScrollY{};
        std::int32_t m_nScreenScrollX{};
        std::int32_t nRShift{};
        std::int32_t nGShift{};
        std::int32_t nBShift{};
        tagRGBQUAD rgbTint{};
        std::int32_t bPointerEnabled{};
        CRect rPointerStorage{};
        CRect m_rLockedRect{};
        CVidCell *m_lastCursor{};
        std::int32_t m_lastCursorFrame{};
        std::int32_t m_lastCursorSequence{};
        std::int32_t m_lastCursorNumber{};
        std::uint32_t m_lastCursorFlags{};
        std::uint32_t m_lastCursorResId{};
        std::byte _pad4[4]{};
        void *m_hwCursor{};
        void *m_hwCursorSurface{};
        std::int32_t nVRamSurfaces{};
        std::byte _pad5[4]{};
        CVidBitmapOpaque m_rgbMasterBitmap{};
    };

    struct CVIDPOLY_VERTEX {
        std::uint16_t x{};
        std::uint16_t y{};
    };

    struct CVidPoly {
        CVIDPOLY_VERTEX *m_pVertices{};
        std::int32_t m_nVertices{};
        std::byte _pad0[4]{};
        void *m_pET{};
        void *m_pAET{};
        void (*m_pDrawHLineFunction)(CVidPoly *, void *, int, int, unsigned int, const CRect *, const CPoint *){};
    };

    struct CTimerWorld {
        std::uint32_t m_gameTime{};
        std::uint8_t m_active{};
        std::uint8_t m_nLastPercentage{};
        std::byte _pad0[2]{};
    };

    struct CVisibilityMap {
        std::uint16_t *m_pMap{};
        std::int32_t m_nMapSize{};
        std::int16_t m_nWidth{};
        std::int16_t m_nHeight{};
        std::uint8_t m_bOutDoor{};
        std::byte _pad0[7]{};
        void *m_pSearchMap{};
        std::array<std::int32_t, 15> m_aCharacterIds{};
        std::byte _pad1[4]{};
        void **m_pVisMapTrees{};
        void *m_pVisMapEllipses{};
    };

    struct CTiledObject {
        std::int32_t m_nWedIndex{};
        std::byte _pad0[4]{};
        CResWED *m_pResWed{};
        std::uint16_t m_wAIState{};
        std::uint16_t m_wRenderState{};
        std::byte _pad1[4]{};
        void *m_posAreaList{};
        CResRef m_resId{};
    };

    struct CTypedPtrListOpaque {
        std::array<std::byte, 56> data{};
    };

    struct CGameAreaNotesOpaque {
        std::array<std::byte, 168> data{};
    };

    struct CInfTileSet {
        std::array<CResTileSet *, 2> tis{};
        CVidTile cVidTile{};
        void *pVRPool{};
        void **pResTiles{};
        std::uint32_t nTiles{};
        std::uint32_t nTileSize{};
    };

    struct CInfGame {
        std::array<std::byte, 0x3FA0> _pad0{};
        CTimerWorld m_worldTime{};
        std::int32_t m_bGameLoaded{};
        std::uint8_t m_bInLoadGame{};
        std::uint8_t m_bInLoadArea{};
        std::uint8_t m_bInIniSpawn{};
        std::byte _pad1[1]{};
        std::uint32_t m_nUniqueAreaID{};
        std::uint32_t m_nAreaFirstObject{};
        std::uint8_t m_bFromNewGame{};
        std::uint8_t m_bInDestroyGame{};
        std::uint8_t m_bAnotherPlayerJoinedGame{};
        std::uint8_t m_bInAreaTransition{};
        std::int32_t m_bStartedDeathSequence{};
        std::array<std::byte, 0x5300> _pad2{};
        CVidCell m_vcLocator{};
        std::array<std::byte, 0x400> _tail{};
    };

    struct CInfinity {
        std::array<CInfTileSet *, 5> pTileSets{};
        CResWED *pResWED{};
        void *pVRPool{};
        CVidMode *pVidMode{};
        std::int32_t bUseDestSrc{};
        std::int32_t bRefreshVRamRect{};
        std::int32_t bInitialized{};
        std::int32_t bWEDDemanded{};
        std::int32_t nOffsetX{};
        std::int32_t nOffsetY{};
        std::int32_t nTilesX{};
        std::int32_t nTilesY{};
        std::int32_t nNewX{};
        std::int32_t nNewY{};
        std::array<std::byte, 0x288> _pad0{};
        std::uint32_t m_nLastTickCount{};
        CPoint m_ptCurrentPosExact{};
        std::int16_t m_autoScrollSpeed{};
        std::byte _pad1[2]{};
        CPoint m_ptScrollDest{};
        std::int32_t m_nScrollAttempts{};
        std::int32_t m_nOldScrollState{};
        std::uint8_t m_nScrollDelay{};
        std::uint8_t m_bMovieBroadcast{};
        std::byte _pad2[2]{};
        std::int32_t m_bStartLightning{};
        std::int32_t m_bStopLightning{};
        std::uint8_t m_lightningStrikeProb{};
        std::byte _pad3[3]{};
        std::uint32_t m_rgbRainColor{};
        std::uint32_t m_rgbLightningGlobalLighting{};
        std::uint32_t m_rgbOverCastGlobalLighting{};
        std::uint32_t m_rgbGlobalLighting{};
        std::uint32_t m_rgbTimeOfDayGlobalLighting{};
        std::uint32_t m_rgbTimeOfDayRainColor{};
        std::int32_t m_updateListenPosition{};
        std::byte _pad4[4]{};
        CGameArea *m_pArea{};
        std::array<std::byte, 0x150> _tail{};
    };

    struct CGameArea {
        std::array<std::byte, 0x200> _pad0{};
        std::uint8_t m_id{};
        std::uint8_t m_nCharacters{};
        std::uint8_t m_nInfravision{};
        std::uint8_t m_bAreaLoaded{};
        CResRef m_resref{};
        CResRef m_restMovieDay{};
        CResRef m_restMovieNight{};
        std::uint8_t m_waterAlpha{};
        std::byte _pad1[3]{};
        CResWED *m_pResWED{};
        CInfGame *m_pGame{};
        std::int32_t m_nScrollState{};
        std::int32_t m_nKeyScrollState{};
        std::int32_t m_bSelectionSquareEnabled{};
        std::int32_t m_bTravelSquare{};
        std::int32_t m_iPickedOnDown{};
        std::array<std::byte, 0x278> _pad2{};
        CRect m_selectSquare{};
        std::int16_t m_rotation{};
        std::byte _pad3[2]{};
        CPoint m_moveDest{};
        std::int32_t m_groupMove{};
        std::array<std::uint8_t, 16> m_terrainTable{};
        std::array<std::uint8_t, 16> m_visibleTerrainTable{};
        std::int32_t m_nAIIndex{};
        std::int32_t m_bInPathSearch{};
        std::uint32_t m_nInitialAreaID{};
        std::uint32_t m_nFirstObject{};
        std::uint32_t m_dwLastProgressRenderTickCount{};
        std::uint32_t m_dwLastProgressMsgTickCount{};
        std::uint8_t m_nRandomMonster{};
        std::byte _pad4[1]{};
        std::int16_t m_nVisibleMonster{};
        std::uint8_t m_bRecentlySaved{};
        std::byte _pad5[3]{};
        std::uint32_t m_nSavedTime{};
        CGameAreaNotesOpaque m_cGameAreaNotes{};
        CInfinity m_cInfinity{};
        std::array<std::byte, 0x150> _pad6{};
        CVisibilityMap m_visibility{};
        std::uint8_t *m_pDynamicHeight{};
        std::int32_t m_startedMusic{};
        std::uint32_t m_startedMusicCounter{};
        std::array<std::byte, 0x2A0> _pad7{};
        CTypedPtrListOpaque m_lTiledObjects{};
        std::array<std::byte, 0x70> _pad8{};
        CPoint m_ptOldViewPos{};
        std::array<std::byte, 0x1A0> _tail{};
    };

    struct CGameSprite {
        std::array<std::byte, 0x540> _pad0{};
        CResRef m_resref{};
        std::uint16_t m_type{};
        std::byte _pad1[2]{};
        std::uint32_t m_expirationTime{};
        std::uint16_t m_huntingRange{};
        std::uint16_t m_followRange{};
        CPoint m_posStart{};
        std::uint32_t m_timeOfDayVisible{};
        std::array<std::byte, 0x34C0> _pad2{};
        CResRef m_currentArea{};
        std::uint8_t m_bGlobal{};
        std::uint8_t m_nModalState{};
        std::byte _pad3[6]{};
        std::array<std::byte, 0x240> _pad4{};
        CVidCell m_spriteEffectVidCell{};
        CVidPalette m_spriteEffectPalette{};
        std::uint32_t m_spriteEffectFlags{};
        std::byte _pad5[4]{};
        CVidCell m_spriteSplashVidCell{};
        CVidPalette m_spriteSplashPalette{};
        std::uint32_t m_spriteSplashFlags{};
        CRect m_rSpriteEffectFX{};
        CPoint m_ptSpriteEffectReference{};
        std::uint8_t m_effectExtendDirection{};
        std::uint8_t m_bEscapingArea{};
        std::byte _pad6[2]{};
        std::int32_t m_animationRunning{};
        std::int32_t m_posZDelta{};
        std::uint8_t m_doBounce{};
        std::uint8_t m_nMirrorImages{};
        std::uint8_t m_bBlur{};
        std::uint8_t m_bInvisible{};
        std::uint8_t m_bSanctuary{};
        std::byte _pad7[3]{};
        std::array<std::byte, 0x79C> _pad8{};
        CPoint m_posExact{};
        CPoint m_posDelta{};
        CPoint m_posDest{};
        CPoint m_posOld{};
        CPoint m_posOldWalk{};
        CPoint m_posLastVisMapEntry{};
        std::array<std::byte, 0x1F0> _pad9{};
        CPoint m_ptBumpedFrom{};
        std::array<std::byte, 0xA4C> _tail{};
    };

    static_assert(std::is_standard_layout_v<CRes>);
    static_assert(std::is_standard_layout_v<CVidTile>);
    static_assert(std::is_standard_layout_v<CInfinity>);
    static_assert(std::is_standard_layout_v<CGameArea>);
    static_assert(std::is_standard_layout_v<CGameSprite>);

    static_assert(sizeof(CPoint) == 0x8);
    static_assert(sizeof(CSize) == 0x8);
    static_assert(sizeof(CRect) == 0x10);
    static_assert(sizeof(CRes) == 0x58);
    static_assert(sizeof(CResRef) == 0x8);
    static_assert(sizeof(CResPVR) == 0x70);
    static_assert(sizeof(CResTileSet) == 0x60);
    static_assert(sizeof(CResTile) == 0x18);
    static_assert(sizeof(CResWED) == 0x88);
    static_assert(sizeof(CVidPalette) == 0x30);
    static_assert(sizeof(CVIDIMG_PALETTEAFFECT) == 0xD0);
    static_assert(sizeof(CVidImage) == 0x100);
    static_assert(sizeof(CVidTile) == 0x110);
    static_assert(sizeof(CVidCell) == 0x138);
    static_assert(sizeof(CVidMode) == 0x318);
    static_assert(sizeof(CVidPoly) == 0x28);
    static_assert(sizeof(CVisibilityMap) == 0x70);
    static_assert(sizeof(CTiledObject) == 0x28);
    static_assert(sizeof(CInfTileSet) == 0x138);
    static_assert(sizeof(CInfGame) == 0x97F8);
    static_assert(sizeof(CInfinity) == 0x498);
    static_assert(sizeof(CGameArea) == 0x1120);
    static_assert(sizeof(CGameSprite) == 0x5388);

    static_assert(offsetof(CRes, pData) == 0x40);
    static_assert(offsetof(CRes, bLoaded) == 0x51);
    static_assert(offsetof(CResTile, pvr) == 0x10);
    static_assert(offsetof(CResWED, pWEDHeader) == 0x58);
    static_assert(offsetof(CVidTile, pRes) == 0x100);
    static_assert(offsetof(CVidCell, m_nCurrentFrame) == 0x118);
    static_assert(offsetof(CVidMode, pPointerVidCell) == 0x160);
    static_assert(offsetof(CVidMode, m_rgbMasterBitmap) == 0x1F8);
    static_assert(offsetof(CVisibilityMap, m_pSearchMap) == 0x18);
    static_assert(offsetof(CTiledObject, m_resId) == 0x20);
    static_assert(offsetof(CInfTileSet, cVidTile) == 0x10);
    static_assert(offsetof(CInfGame, m_worldTime) == 0x3FA0);
    static_assert(offsetof(CInfGame, m_vcLocator) == 0x92C0);
    static_assert(offsetof(CInfinity, pTileSets) == 0x0);
    static_assert(offsetof(CInfinity, m_ptCurrentPosExact) == 0x2F4);
    static_assert(offsetof(CInfinity, m_pArea) == 0x340);
    static_assert(offsetof(CGameArea, m_resref) == 0x204);
    static_assert(offsetof(CGameArea, m_pResWED) == 0x220);
    static_assert(offsetof(CGameArea, m_cInfinity) == 0x5C8);
    static_assert(offsetof(CGameArea, m_visibility) == 0xBB0);
    static_assert(offsetof(CGameArea, m_lTiledObjects) == 0xED0);
    static_assert(offsetof(CGameArea, m_ptOldViewPos) == 0xF78);
    static_assert(offsetof(CGameSprite, m_resref) == 0x540);
    static_assert(offsetof(CGameSprite, m_currentArea) == 0x3A20);
    static_assert(offsetof(CGameSprite, m_spriteEffectVidCell) == 0x3C70);
    static_assert(offsetof(CGameSprite, m_rSpriteEffectFX) == 0x3F4C);
    static_assert(offsetof(CGameSprite, m_posExact) == 0x4714);
    static_assert(offsetof(CGameSprite, m_ptBumpedFrom) == 0x4934);
}
