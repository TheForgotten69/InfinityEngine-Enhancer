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

    void test_config_parsing() {
        const auto tempPath = std::filesystem::current_path() / "InfinityEngine-Enhancer-test.ini";
        {
            std::ofstream out(tempPath, std::ios::trunc);
            expect_true(static_cast<bool>(out), "Config test fixture should be writable");
            out << "[Core]\n";
            out << "VerboseLogs = true\n\n";
            out << "EnableShaderTracing = true\n\n";
            out << "EnableSpriteTcScaleInjection = true\n\n";
            out << "EnableBatchHighlightProbe = true\n";
            out << "EnableBatchSuppressProbe = false\n";
            out << "EnableGlProgramSuppressProbe = true\n";
            out << "EnableGlTextureSuppressProbe = true\n";
            out << "BatchHighlightProgram = 18\n";
            out << "BatchHighlightTextureId = 20\n\n";
            out << "BatchSuppressMinScreenWidth = 60\n";
            out << "BatchSuppressMaxScreenWidth = 120\n";
            out << "BatchSuppressMinScreenHeight = 90\n";
            out << "BatchSuppressMaxScreenHeight = 140\n\n";
            out << "BatchSuppressMinCenterX = 400\n";
            out << "BatchSuppressMaxCenterX = 1200\n";
            out << "BatchSuppressMinCenterY = 100\n";
            out << "BatchSuppressMaxCenterY = 700\n\n";
            out << "GlProgramSuppress = 18\n\n";
            out << "GlTextureSuppressProgram = 3\n";
            out << "GlTextureSuppressTexture = 20\n\n";
            out << "ShaderTraceRuntimeProgramFilter = 3\n";
            out << "ShaderTraceRuntimeTextureFilter = 2\n\n";
            out << "EnableSpriteBodyFsrPrototype = true\n";
            out << "SpriteBodyProgram = 3\n";
            out << "SpriteBodyTexture = 2\n";
            out << "SpriteBodyInputScale = 0.667\n";
            out << "SpriteBodyEnableRcas = true\n";
            out << "SpriteBodyRcasSharpness = 0.20\n";
            out << "SpriteBodyDebugView = 2\n\n";
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
        expect_true(cfg.enableShaderTracing, "Shader tracing flag should parse");
        expect_true(cfg.enableSpriteTcScaleInjection, "Sprite tc-scale injection flag should parse");
        expect_true(cfg.enableBatchHighlightProbe, "Batch highlight probe flag should parse");
        expect_true(!cfg.enableBatchSuppressProbe, "Batch suppress probe flag should parse");
        expect_true(cfg.enableGlProgramSuppressProbe, "GL program suppress probe flag should parse");
        expect_true(cfg.enableGlTextureSuppressProbe, "GL texture suppress probe flag should parse");
        expect_eq(cfg.batchHighlightProgram, 18, "Batch highlight program should parse");
        expect_eq(cfg.batchHighlightTextureId, 20, "Batch highlight texture id should parse");
        expect_eq(cfg.batchSuppressMinScreenWidth, 60, "Batch suppress min screen width should parse");
        expect_eq(cfg.batchSuppressMaxScreenWidth, 120, "Batch suppress max screen width should parse");
        expect_eq(cfg.batchSuppressMinScreenHeight, 90, "Batch suppress min screen height should parse");
        expect_eq(cfg.batchSuppressMaxScreenHeight, 140, "Batch suppress max screen height should parse");
        expect_eq(cfg.batchSuppressMinCenterX, 400, "Batch suppress min center X should parse");
        expect_eq(cfg.batchSuppressMaxCenterX, 1200, "Batch suppress max center X should parse");
        expect_eq(cfg.batchSuppressMinCenterY, 100, "Batch suppress min center Y should parse");
        expect_eq(cfg.batchSuppressMaxCenterY, 700, "Batch suppress max center Y should parse");
        expect_eq(cfg.glProgramSuppress, 18, "GL program suppress target should parse");
        expect_eq(cfg.glTextureSuppressProgram, 3, "GL texture suppress program should parse");
        expect_eq(cfg.glTextureSuppressTexture, 20, "GL texture suppress texture should parse");
        expect_eq(cfg.shaderTraceRuntimeProgramFilter, 3, "Shader trace runtime program filter should parse");
        expect_eq(cfg.shaderTraceRuntimeTextureFilter, 2, "Shader trace runtime texture filter should parse");
        expect_true(cfg.enableSpriteBodyFsrPrototype, "Sprite body FSR prototype flag should parse");
        expect_eq(cfg.spriteBodyProgram, 3, "Sprite body program should parse");
        expect_eq(cfg.spriteBodyTexture, 2, "Sprite body texture should parse");
        expect_eq(cfg.spriteBodyInputScale, 0.667f, "Sprite body input scale should parse");
        expect_true(cfg.spriteBodyEnableRcas, "Sprite body RCAS flag should parse");
        expect_eq(cfg.spriteBodyRcasSharpness, 0.20f, "Sprite body RCAS sharpness should parse");
        expect_eq(cfg.spriteBodyDebugView, 2, "Sprite body debug view should parse");
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
