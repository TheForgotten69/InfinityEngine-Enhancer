#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

#include "iee/core/config.h"
#include "iee/core/pattern_scanner.h"
#include "iee/game/build_manifest.h"
#include "iee/game/eeex_doc_layouts_x64.h"
#include "iee/game/file_formats.h"
#include "iee/game/runtime_types_x64.h"
#include "iee/game/tile_upscale.h"

namespace {
    int g_failures = 0;

    void expect_true(bool condition, std::string_view message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++g_failures;
        }
    }

    template<typename T, typename U>
    void expect_eq(const T &actual, const U &expected, std::string_view message) {
        if (!(actual == expected)) {
            std::cerr << "FAIL: " << message << " (actual=" << actual << ", expected=" << expected << ")\n";
            ++g_failures;
        }
    }

    void write_rel32_instruction(std::byte *instruction,
                                 std::uint8_t opcode,
                                 std::size_t displacementOffset,
                                 std::size_t instructionSize,
                                 const std::byte *target) {
        instruction[0] = static_cast<std::byte>(opcode);
        const auto displacement = static_cast<std::intptr_t>(target - (instruction + instructionSize));
        const auto rel32 = static_cast<std::int32_t>(displacement);
        std::memcpy(instruction + displacementOffset, &rel32, sizeof(rel32));
    }

    void test_parse_ida_pattern() {
        std::vector<std::byte> bytes;
        std::vector<bool> mask;
        expect_true(iee::core::parse_ida_pattern("48 8B ? ? 89 54 24 ??", bytes, mask),
                    "IDA patterns should support wildcards");
        expect_eq(bytes.size(), std::size_t{8}, "Parsed pattern size should match token count");
        expect_true(mask[0] && mask[1] && !mask[2] && !mask[3] && mask[4] && mask[5] && mask[6] && !mask[7],
                    "Pattern mask should mark wildcard positions");
    }

    void test_rel32_target_checked() {
        std::array<std::byte, 32> callBytes{};
        auto *callInstruction = callBytes.data() + 4;
        auto *callTarget = callBytes.data() + 24;
        write_rel32_instruction(callInstruction, 0xE8, 1, 5, callTarget);

        expect_true(iee::core::rel32_target_checked(callInstruction, 0xE8, 1, 5) == callTarget,
                    "CALL rel32 decoding should return the target");
        expect_true(iee::core::rel32_target_checked(callInstruction, 0xE9, 1, 5) == nullptr,
                    "Opcode validation should reject mismatched CALL instructions");

        std::array<std::byte, 32> jmpBytes{};
        auto *jmpInstruction = jmpBytes.data() + 8;
        auto *jmpTarget = jmpBytes.data() + 28;
        write_rel32_instruction(jmpInstruction, 0xE9, 1, 5, jmpTarget);

        expect_true(iee::core::rel32_target_checked(jmpInstruction, 0xE9, 1, 5) == jmpTarget,
                    "JMP rel32 decoding should return the target");
    }

    void test_manifest_loading() {
        const auto &manifest = iee::game::current_manifest();
        expect_true(manifest.validate(), "Current build manifest should validate");
        expect_eq(manifest.fallbacks.loadArea, std::uintptr_t{0x27E710}, "LoadArea fallback RVA should match");
        expect_eq(manifest.fallbacks.renderTexture, std::uintptr_t{0x4247E0},
                  "RenderTexture fallback RVA should match");

        const auto found = iee::game::find_manifest("BGEE 2.6.6.x");
        expect_true(found.has_value(), "Known build manifest should be discoverable by id");
        if (found) {
            expect_eq(found->get().offsets.tisHeaderTileDimension, std::uintptr_t{0x14},
                      "Manifest should carry the TIS header tile dimension offset");
        }
    }

    void test_runtime_type_layouts() {
        using namespace iee::game;

        expect_eq(sizeof(CRes), std::size_t{0x58}, "CRes should match the curated x64 layout");
        expect_eq(sizeof(CResWED), std::size_t{0x88}, "CResWED should match the curated x64 layout");
        expect_eq(sizeof(CVidTile), std::size_t{0x110}, "CVidTile should match the curated x64 layout");
        expect_eq(sizeof(CVidCell), std::size_t{0x138}, "CVidCell should match the curated x64 layout");
        expect_eq(sizeof(CVidMode), std::size_t{0x318}, "CVidMode should match the curated x64 layout");
        expect_eq(sizeof(CVisibilityMap), std::size_t{0x70},
                  "CVisibilityMap should match the curated x64 layout");
        expect_eq(sizeof(CInfTileSet), std::size_t{0x138}, "CInfTileSet should match the curated x64 layout");
        expect_eq(sizeof(CInfGame), std::size_t{0x97F8}, "CInfGame should match the curated x64 layout");
        expect_eq(sizeof(CInfinity), std::size_t{0x498}, "CInfinity should match the curated x64 layout");
        expect_eq(sizeof(CGameArea), std::size_t{0x1120}, "CGameArea should match the curated x64 layout");
        expect_eq(sizeof(CGameSprite), std::size_t{0x5388}, "CGameSprite should match the curated x64 layout");

        expect_eq(offsetof(CRes, pData), std::size_t{0x40}, "CRes::pData offset should match EEex docs");
        expect_eq(offsetof(CRes, bLoaded), std::size_t{0x51}, "CRes::bLoaded offset should match EEex docs");
        expect_eq(offsetof(CVidTile, pRes), std::size_t{0x100}, "CVidTile::pRes offset should match EEex docs");
        expect_eq(offsetof(CInfGame, m_worldTime), std::size_t{0x3FA0},
                  "CInfGame::m_worldTime offset should match EEex docs");
        expect_eq(offsetof(CInfGame, m_vcLocator), std::size_t{0x92C0},
                  "CInfGame::m_vcLocator offset should match EEex docs");
        expect_eq(offsetof(CInfinity, m_ptCurrentPosExact), std::size_t{0x2F4},
                  "CInfinity::m_ptCurrentPosExact offset should match EEex docs");
        expect_eq(offsetof(CInfinity, m_pArea), std::size_t{0x340},
                  "CInfinity::m_pArea offset should match EEex docs");
        expect_eq(offsetof(CGameArea, m_resref), std::size_t{0x204},
                  "CGameArea::m_resref offset should match EEex docs");
        expect_eq(offsetof(CGameArea, m_cInfinity), std::size_t{0x5C8},
                  "CGameArea::m_cInfinity offset should match EEex docs");
        expect_eq(offsetof(CGameArea, m_lTiledObjects), std::size_t{0xED0},
                  "CGameArea::m_lTiledObjects offset should match EEex docs");
        expect_eq(offsetof(CGameSprite, m_currentArea), std::size_t{0x3A20},
                  "CGameSprite::m_currentArea offset should match EEex docs");
        expect_eq(offsetof(CGameSprite, m_spriteEffectVidCell), std::size_t{0x3C70},
                  "CGameSprite::m_spriteEffectVidCell offset should match EEex docs");
        expect_eq(offsetof(CGameSprite, m_posExact), std::size_t{0x4714},
                  "CGameSprite::m_posExact offset should match EEex docs");
    }

    void test_file_format_layouts() {
        using namespace iee::game;

        expect_eq(sizeof(PVRTextureHeaderV3), std::size_t{0x34}, "PVRTextureHeaderV3 should match EEex docs");
        expect_eq(sizeof(ResFixedHeader_st), std::size_t{0x14}, "ResFixedHeader_st should match EEex docs");
        expect_eq(sizeof(TisFileHeader), std::size_t{0x18},
                  "TisFileHeader should carry the locally validated tile-dimension field");
        expect_eq(sizeof(WED_WedHeader_st), std::size_t{0x2C}, "WED_WedHeader_st should match EEex docs");
        expect_eq(sizeof(WED_LayerHeader_st), std::size_t{0x18}, "WED_LayerHeader_st should match EEex docs");
        expect_eq(sizeof(WED_PolyHeader_st), std::size_t{0x14}, "WED_PolyHeader_st should match EEex docs");
        expect_eq(sizeof(WED_PolyList_st), std::size_t{0x12}, "WED_PolyList_st should match EEex docs");
        expect_eq(sizeof(WED_PolyPoint_st), std::size_t{0x4}, "WED_PolyPoint_st should match EEex docs");
        expect_eq(sizeof(WED_ScreenSectionList), std::size_t{0x4},
                  "WED_ScreenSectionList should match EEex docs");
        expect_eq(sizeof(WED_TileData_st), std::size_t{0xA}, "WED_TileData_st should match EEex docs");
        expect_eq(sizeof(WED_TiledObject_st), std::size_t{0x1A}, "WED_TiledObject_st should match EEex docs");
        expect_eq(sizeof(bamHeader_st), std::size_t{0x18}, "bamHeader_st should match EEex docs");
        expect_eq(sizeof(BAMHEADERV2), std::size_t{0x20}, "BAMHEADERV2 should match EEex docs");
        expect_eq(sizeof(frame), std::size_t{0x18}, "frame should match EEex docs");
        expect_eq(sizeof(frameTableEntry_st), std::size_t{0xC}, "frameTableEntry_st should match EEex docs");
        expect_eq(sizeof(st_tiledef), std::size_t{0x18}, "st_tiledef should match EEex docs");

        expect_eq(offsetof(PVRTextureHeaderV3, u32MetaDataSize), std::size_t{0x30},
                  "PVRTextureHeaderV3::u32MetaDataSize offset should match EEex docs");
        expect_eq(offsetof(TisFileHeader, tileDimension), std::size_t{0x14},
                  "TisFileHeader::tileDimension offset should match the validated runtime fact");
        expect_eq(offsetof(WED_WedHeader_st, dwFlags), std::size_t{0x28},
                  "WED_WedHeader_st::dwFlags offset should match EEex docs");
        expect_eq(offsetof(WED_LayerHeader_st, nOffsetToTileData), std::size_t{0x10},
                  "WED_LayerHeader_st::nOffsetToTileData offset should match EEex docs");
        expect_eq(offsetof(WED_TileData_st, bFlags), std::size_t{0x6},
                  "WED_TileData_st::bFlags offset should match EEex docs");
        expect_eq(offsetof(bamHeader_st, nFrameListOffset), std::size_t{0x14},
                  "bamHeader_st::nFrameListOffset offset should match EEex docs");
        expect_eq(offsetof(frameTableEntry_st, ___u4), std::size_t{0x8},
                  "frameTableEntry_st::___u4 offset should match EEex docs");
    }

    void test_eeex_doc_layout_maps() {
        using namespace iee::game::eeex_doc;
        const auto find_field = [](const auto &fields, std::string_view name) -> const FieldDesc * {
            for (const auto &field: fields) {
                if (field.name == name) {
                    return &field;
                }
            }
            return nullptr;
        };

        expect_eq(CInfGameLayout.size, std::uint32_t{0x97F8}, "CInfGame doc layout size should match EEex docs");
        expect_eq(CInfGameLayout.fieldCount, std::uint32_t{128},
                  "CInfGame doc layout should expose every EEex field row");
        expect_eq(CInfinityLayout.size, std::uint32_t{0x498}, "CInfinity doc layout size should match EEex docs");
        expect_eq(CInfinityLayout.fieldCount, std::uint32_t{90},
                  "CInfinity doc layout should expose every EEex field row");
        expect_eq(CGameAreaLayout.size, std::uint32_t{0x1120}, "CGameArea doc layout size should match EEex docs");
        expect_eq(CGameAreaLayout.fieldCount, std::uint32_t{105},
                  "CGameArea doc layout should expose every EEex field row");
        expect_eq(CGameSpriteLayout.size, std::uint32_t{0x5388},
                  "CGameSprite doc layout size should match EEex docs");
        expect_eq(CGameSpriteLayout.fieldCount, std::uint32_t{339},
                  "CGameSprite doc layout should expose every EEex field row");
        expect_eq(CGameAnimationTypeLayout.fieldCount, std::uint32_t{24},
                  "CGameAnimationType doc layout should expose every EEex field row");
        expect_eq(CGameOptionsLayout.fieldCount, std::uint32_t{153},
                  "CGameOptions doc layout should expose every EEex field row");
        expect_eq(CVidModeLayout.fieldCount, std::uint32_t{42},
                  "CVidMode doc layout should expose every EEex field row");

        const auto *animationShaderField = find_field(CGameAnimationTypeFields, "m_bUseSpriteShader");
        expect_true(animationShaderField != nullptr,
                    "CGameAnimationType should expose the sprite-shader field from EEex docs");
        if (animationShaderField) {
            expect_eq(animationShaderField->offset, std::uint32_t{0x20},
                      "CGameAnimationType::m_bUseSpriteShader offset should match EEex docs");
        }

        const auto *optionShaderField = find_field(CGameOptionsFields, "m_bUseSpriteShader");
        expect_true(optionShaderField != nullptr,
                    "CGameOptions should expose the sprite-shader option from EEex docs");
        if (optionShaderField) {
            expect_eq(optionShaderField->offset, std::uint32_t{0x238},
                      "CGameOptions::m_bUseSpriteShader offset should match EEex docs");
        }

        const auto *areaInfinityField = find_field(CGameAreaFields, "m_cInfinity");
        expect_true(areaInfinityField != nullptr,
                    "CGameArea should expose the embedded CInfinity field from EEex docs");
        if (areaInfinityField) {
            expect_eq(areaInfinityField->offset, std::uint32_t{0x5C8},
                      "CGameArea::m_cInfinity offset should match EEex docs");
        }

        const auto *areaTiledObjectField = find_field(CGameAreaFields, "m_lTiledObjects");
        expect_true(areaTiledObjectField != nullptr,
                    "CGameArea should expose the tiled-object list from EEex docs");
        if (areaTiledObjectField) {
            expect_eq(areaTiledObjectField->offset, std::uint32_t{0xED0},
                      "CGameArea::m_lTiledObjects offset should match EEex docs");
        }
        expect_true(CGameSpriteFields[0].name == "baseclass_0",
                    "CGameSprite doc layout should begin at the documented baseclass");
        expect_true(CGameSpriteFields.back().name == "m_bOutline",
                    "CGameSprite doc layout should include the final documented field");
    }

    void test_config_parsing() {
        const auto tempPath = std::filesystem::current_path() / "InfinityEngine-Enhancer-test.ini";
        {
            std::ofstream out(tempPath, std::ios::trunc);
            expect_true(static_cast<bool>(out), "Config test fixture should be writable");
            out << "[Core]\n";
            out << "VerboseLogs = true\n\n";
            out << "[Rendering]\n";
            out << "EnableAnisotropicFiltering = false\n";
            out << "MaxAnisotropy = 4.0\n";
            out << "LODBias = -0.5\n\n";
            out << "[Addresses]\n";
            out << "FallbackLoadAreaRVA = 0x27E710\n";
            out << "FallbackRenderTextureRVA = 4343776\n";
        }

        iee::core::EngineConfig cfg{};
        expect_true(iee::core::ConfigManager::load(tempPath, cfg), "ConfigManager::load should parse a valid INI");
        expect_true(cfg.enableVerboseLogging, "Verbose logging flag should parse");
        expect_true(!cfg.enableAnisotropicFiltering, "Rendering bool should parse");
        expect_eq(cfg.maxAnisotropy, 4.0f, "Floating-point values should parse");
        expect_eq(cfg.lodBias, -0.5f, "Negative float values should parse");
        expect_eq(cfg.cachedLoadAreaRVA, std::uintptr_t{0x27E710}, "Hex fallback load RVA should parse");
        expect_eq(cfg.cachedRenderTextureRVA, std::uintptr_t{4343776}, "Decimal fallback render RVA should parse");

        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    }

    iee::game::TileInfo make_tile_info(std::uint32_t tileDimension,
                                       int texId,
                                       int u,
                                       int v,
                                       bool includeHeader = true,
                                       bool *outLinearFlag = nullptr) {
        const auto &manifest = iee::game::current_manifest();

        static std::vector<std::byte> vidTileStorage;
        static std::vector<std::byte> tilesetStorage;
        static iee::game::TisFileHeader header;
        static std::array<iee::game::PVRZTileEntry, 2> table;
        static iee::game::CResTile resource;

        vidTileStorage.assign(manifest.offsets.vidTileResource + sizeof(iee::game::CResTile *), std::byte{0});
        tilesetStorage.assign(manifest.offsets.tisLinearTilesFlag + sizeof(std::int32_t), std::byte{0});

        auto *tileset = reinterpret_cast<iee::game::CResTileSet *>(tilesetStorage.data());
        header = {};
        header.tileDimension = tileDimension;

        table[0] = iee::game::PVRZTileEntry{texId, 0, 0};
        table[1] = iee::game::PVRZTileEntry{texId, u, v};
        resource = {};
        resource.tis = tileset;
        resource.tileIndex = 0;

        tileset->baseclass_0.pData = table.data();
        tileset->baseclass_0.nSize = static_cast<std::uint32_t>(sizeof(iee::game::PVRZTileEntry));
        tileset->baseclass_0.nCount = static_cast<std::uint32_t>(table.size());
        tileset->h = includeHeader ? &header : nullptr;

        if (outLinearFlag) {
            const auto linearValue = *outLinearFlag ? 1 : 0;
            std::memcpy(tilesetStorage.data() + manifest.offsets.tisLinearTilesFlag,
                        &linearValue,
                        sizeof(linearValue));
        }

        auto *resourcePtr = &resource;
        std::memcpy(vidTileStorage.data() + manifest.offsets.vidTileResource, &resourcePtr, sizeof(resourcePtr));

        iee::game::TileInfo info{};
        const auto demand_passthrough = +[](void *p) -> void * { return p; };
        expect_true(iee::game::get_tile_info(vidTileStorage.data(), manifest, info, demand_passthrough),
                    "get_tile_info should decode the synthetic CVidTile payload");
        return info;
    }

    void test_tis_header_dimension_decoding() {
        const auto &manifest = iee::game::current_manifest();
        auto tileInfo = make_tile_info(iee::game::TisTileDimensions::Upscaled4x,
                                       15000,
                                       static_cast<int>(iee::game::TisTileDimensions::Upscaled4x),
                                       0);

        const auto tileDimension = iee::game::get_tis_header_tile_dimension(tileInfo, manifest);
        expect_true(tileDimension.has_value(), "TIS header tile dimension should be readable");
        if (tileDimension) {
            expect_eq(*tileDimension, std::uint32_t{0x100}, "4x tile dimension should decode from the header");
        }

        const auto detection = iee::game::detect_scale_from_tis_header(tileInfo, manifest);
        expect_true(detection.has_value(), "Known tile dimensions should resolve from the header");
        if (detection) {
            expect_eq(detection->scaleFactor, 4, "Header-based detection should map 0x100 to 4x scale");
            expect_true(detection->source == iee::game::ScaleDetectionSource::TisHeader,
                        "Header-based detection should report the correct source");
        }
    }

    void test_scale_selection_precedence() {
        const auto &manifest = iee::game::current_manifest();

        auto standardInfo = make_tile_info(iee::game::TisTileDimensions::Standard,
                                           20000,
                                           static_cast<int>(iee::game::TisTileDimensions::Standard),
                                           0);
        const auto standardDetection = iee::game::detect_preferred_scale_hint(standardInfo, 20000, manifest);
        expect_true(standardDetection.has_value(), "Header-first detection should produce a scale hint");
        if (standardDetection) {
            expect_eq(standardDetection->scaleFactor, 1, "Standard header values should win over heuristics");
            expect_true(standardDetection->source == iee::game::ScaleDetectionSource::TisHeader,
                        "Standard header values should report TIS-header provenance");
        }

        auto tableOnlyInfo = make_tile_info(0x80,
                                            20000,
                                            static_cast<int>(iee::game::TisTileDimensions::Upscaled4x),
                                            0,
                                            false);
        const auto tableDetection = iee::game::detect_preferred_scale_hint(tableOnlyInfo, 20000, manifest);
        expect_true(tableDetection.has_value(), "Table-derived detection should be used when the header is missing");
        if (tableDetection) {
            expect_eq(tableDetection->scaleFactor, 4, "Table-derived detection should detect 4x tiles");
            expect_true(tableDetection->source == iee::game::ScaleDetectionSource::TileTable,
                        "Fallback should prefer deterministic table provenance over heuristics");
        }

        auto heuristicInfo = make_tile_info(0x80, 20000, 4096, 4096);
        heuristicInfo.header = nullptr;
        heuristicInfo.runtimeTileDimension = 1;
        const auto heuristicDetection = iee::game::detect_preferred_scale_hint(heuristicInfo, 20000, manifest);
        expect_true(heuristicDetection.has_value(), "Heuristics should still exist as a final fallback");
        if (heuristicDetection) {
            expect_eq(heuristicDetection->scaleFactor, 4, "Heuristic fallback should still detect upscaled tiles");
            expect_true(heuristicDetection->source == iee::game::ScaleDetectionSource::Heuristic,
                        "Final fallback should report heuristic provenance");
        }

        bool linearFlag = true;
        auto linearInfo = make_tile_info(iee::game::TisTileDimensions::Upscaled4x,
                                         12000,
                                         static_cast<int>(iee::game::TisTileDimensions::Upscaled4x),
                                         0,
                                         true,
                                         &linearFlag);
        expect_true(iee::game::get_tis_linear_tiles_flag(linearInfo.tileset, manifest),
                    "The manifest linear-tiles offset should be readable from synthetic data");
    }
}

int main() {
    test_parse_ida_pattern();
    test_rel32_target_checked();
    test_manifest_loading();
    test_runtime_type_layouts();
    test_file_format_layouts();
    test_eeex_doc_layout_maps();
    test_config_parsing();
    test_tis_header_dimension_decoding();
    test_scale_selection_precedence();

    if (g_failures != 0) {
        std::cerr << g_failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All InfinityEngine-Enhancer native tests passed\n";
    return 0;
}
