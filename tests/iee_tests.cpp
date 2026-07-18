#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iee/core/config.h"
#include "iee/core/pattern_scanner.h"
#include "iee/core/performance_samples.h"
#include "iee/features/tile_render.h"
#include "iee/game/are_animations.h"
#include "iee/game/area_texture.h"
#include "iee/game/object_statics.h"
#include "iee/game/build_manifest.h"
#include "iee/game/dds_texture.h"
#include "iee/game/eeex_doc_layouts_x64.h"
#include "iee/game/file_formats.h"
#include "iee/game/runtime_types_x64.h"
#include "iee/game/shader_override.h"
#include "iee/game/tile_upscale.h"
#include "iee/game/tis_palette.h"
#include "iee/game/wed_runtime.h"

namespace {
int g_failures = 0;

void expect_true(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
  }
}

template <typename T, typename U>
void expect_eq(const T& actual, const U& expected, std::string_view message) {
  if (!(actual == expected)) {
    std::cerr << "FAIL: " << message << " (actual=" << actual << ", expected=" << expected << ")\n";
    ++g_failures;
  }
}

void write_rel32_instruction(std::byte* instruction, std::uint8_t opcode,
                             std::size_t displacementOffset, std::size_t instructionSize,
                             const std::byte* target) {
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
  expect_true(
      mask[0] && mask[1] && !mask[2] && !mask[3] && mask[4] && mask[5] && mask[6] && !mask[7],
      "Pattern mask should mark wildcard positions");
}

void test_unique_pattern_matching() {
  const std::array<std::byte, 8> haystack{
      std::byte{0x48}, std::byte{0x8B}, std::byte{0x01}, std::byte{0x90},
      std::byte{0x48}, std::byte{0x8B}, std::byte{0x02}, std::byte{0x90},
  };
  const std::array<std::byte, 3> needle{
      std::byte{0x48},
      std::byte{0x8B},
      std::byte{0x00},
  };
  const std::vector<bool> wildcardMask{true, true, false};

  const auto ambiguous = iee::core::find_pattern_unique(haystack, needle, wildcardMask);
  expect_eq(ambiguous.count, std::size_t{2},
            "Ambiguous signatures should report two-or-more matches");
  expect_true(!ambiguous.unique(), "Ambiguous signatures must not be accepted as hook targets");

  const std::array<std::byte, 3> exactNeedle{
      std::byte{0x48},
      std::byte{0x8B},
      std::byte{0x01},
  };
  const std::vector<bool> exactMask{true, true, true};
  const auto unique = iee::core::find_pattern_unique(haystack, exactNeedle, exactMask);
  expect_eq(unique.count, std::size_t{1}, "A single signature occurrence should be reported once");
  expect_true(unique.unique() && unique.address == haystack.data(),
              "A unique signature should return its exact address");
}

void test_detour_tolerant_matching() {
  using iee::core::matches_past_prologue;

  // 21-byte RenderTexture signature; a 14-byte absolute-jump detour (FF 25 ...)
  // clobbered bytes 0..13, the rest is the original tail (see the 2.7.3 log).
  const std::array<std::uint8_t, 21> pattern{0x48, 0x8B, 0xC4, 0x44, 0x89, 0x48, 0x20,
                                             0x48, 0x83, 0xEC, 0x48, 0x48, 0x89, 0x58,
                                             0x08, 0x8B, 0xDA, 0x48, 0x89, 0x68, 0x10};
  std::array<std::uint8_t, 21> live = pattern;
  const std::array<std::uint8_t, 14> detour{0xFF, 0x25, 0x02, 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x70, 0xE7, 0x92, 0x42, 0xF9, 0x7F};
  std::copy(detour.begin(), detour.end(), live.begin());

  const auto toBytes = [](const auto& src) {
    std::vector<std::byte> out(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) out[i] = std::byte{src[i]};
    return out;
  };
  const auto needle = toBytes(pattern);
  const auto haystack = toBytes(live);
  const std::vector<bool> mask(pattern.size(), true);

  expect_true(matches_past_prologue(haystack, needle, mask, 16, 4),
              "A detoured prologue with an intact tail should be accepted");

  // Corrupt a tail byte: the function is genuinely different -> reject.
  auto corrupted = haystack;
  corrupted[18] = std::byte{0xEE};
  expect_true(!matches_past_prologue(corrupted, needle, mask, 16, 4),
              "A mismatched tail byte must reject the candidate");

  // Too few verifiable tail bytes -> reject (cannot pass on prologue alone).
  expect_true(!matches_past_prologue(haystack, needle, mask, 16, 8),
              "Fewer verified tail bytes than required must reject");

  // A wholly different tail (e.g. wrong function at the RVA) -> reject.
  std::array<std::uint8_t, 21> other = live;
  for (std::size_t i = 16; i < other.size(); ++i) other[i] = 0x00;
  expect_true(!matches_past_prologue(toBytes(other), needle, mask, 16, 4),
              "An unrelated tail must reject the candidate");
}

void test_rel32_target_checked() {
  std::array<std::byte, 32> callBytes{};
  auto* callInstruction = callBytes.data() + 4;
  auto* callTarget = callBytes.data() + 24;
  write_rel32_instruction(callInstruction, 0xE8, 1, 5, callTarget);

  expect_true(iee::core::rel32_target_checked(callInstruction, 0xE8, 1, 5) == callTarget,
              "CALL rel32 decoding should return the target");
  expect_true(iee::core::rel32_target_checked(callInstruction, 0xE9, 1, 5) == nullptr,
              "Opcode validation should reject mismatched CALL instructions");

  std::array<std::byte, 32> jmpBytes{};
  auto* jmpInstruction = jmpBytes.data() + 8;
  auto* jmpTarget = jmpBytes.data() + 28;
  write_rel32_instruction(jmpInstruction, 0xE9, 1, 5, jmpTarget);

  expect_true(iee::core::rel32_target_checked(jmpInstruction, 0xE9, 1, 5) == jmpTarget,
              "JMP rel32 decoding should return the target");
}

void test_manifest_loading() {
  const auto& manifest = iee::game::current_manifest();
  expect_true(manifest.validate(), "Current build manifest should validate");
  expect_true(manifest.executableVersion.matches(2, 6, 6, 0),
              "Current manifest should require a BGEE 2.6.6 executable");
  expect_true(manifest.executableVersion.matches(2, 6, 6, 999),
              "The documented 2.6.6.x manifest should explicitly accept any revision");
  expect_true(iee::game::supports_product_name(manifest, "Baldur's Gate: Enhanced Edition"),
              "BGEE product-name punctuation should normalize safely");
  expect_true(!iee::game::supports_product_name(manifest, "Baldur's Gate II: Enhanced Edition"),
              "A sibling Infinity Engine game must not match the BGEE manifest");
  expect_eq(manifest.referenceRvas.loadArea, std::uintptr_t{0x27E710},
            "LoadArea reference RVA should match");
  expect_eq(manifest.referenceRvas.renderTexture, std::uintptr_t{0x4247E0},
            "RenderTexture reference RVA should match");

  const auto found = iee::game::find_manifest("BGEE 2.6.6.x");
  expect_true(found.has_value(), "Known build manifest should be discoverable by id");
  if (found) {
    expect_eq(found->get().offsets.tisHeaderTileDimension, std::uintptr_t{0x14},
              "Manifest should carry the TIS header tile dimension offset");
  }
  expect_true(iee::game::find_manifest_for_version(2, 6, 6, 123).has_value(),
              "BGEE 2.6.6 should resolve by executable version");

  const auto found273 = iee::game::find_manifest("BGEE 2.7.3.x");
  expect_true(found273.has_value(), "2.7.3 manifest should be discoverable by id");
  if (found273) {
    expect_true(found273->get().validate(), "2.7.3 manifest should validate");
    expect_eq(found273->get().referenceRvas.loadArea, std::uintptr_t{0x27EBD0},
              "2.7.3 LoadArea reference RVA should match the offline scan");
    expect_eq(found273->get().referenceRvas.renderTexture, std::uintptr_t{0x4257C0},
              "2.7.3 RenderTexture reference RVA should match the offline scan");
  }
  expect_true(iee::game::find_manifest_for_version(2, 7, 3, 0).has_value(),
              "BGEE 2.7.3.0 should resolve by executable version");
  expect_true(iee::game::find_manifest_for_version(2, 7, 3, 42).has_value(),
              "BGEE 2.7.3.x should accept any revision");
  expect_true(!iee::game::find_manifest_for_version(2, 7, 2, 0).has_value(),
              "Adjacent unknown 2.7.2 must fail closed");
  expect_true(!iee::game::find_manifest_for_version(2, 7, 4, 0).has_value(),
              "Adjacent unknown 2.7.4 must fail closed");
  expect_true(!iee::game::find_manifest_for_version(2, 8, 0, 0).has_value(),
              "Unknown BGEE 2.8 builds must fail closed until validated");
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
  expect_eq(sizeof(CInfTileSet), std::size_t{0x138},
            "CInfTileSet should match the curated x64 layout");
  expect_eq(sizeof(CInfGame), std::size_t{0x97F8}, "CInfGame should match the curated x64 layout");
  expect_eq(sizeof(CInfinity), std::size_t{0x498}, "CInfinity should match the curated x64 layout");
  expect_eq(sizeof(CGameArea), std::size_t{0x1120},
            "CGameArea should match the curated x64 layout");
  expect_eq(sizeof(CGameSprite), std::size_t{0x5388},
            "CGameSprite should match the curated x64 layout");

  expect_eq(offsetof(CRes, pData), std::size_t{0x40}, "CRes::pData offset should match EEex docs");
  expect_eq(offsetof(CRes, bLoaded), std::size_t{0x51},
            "CRes::bLoaded offset should match EEex docs");
  expect_eq(offsetof(CVidTile, pRes), std::size_t{0x100},
            "CVidTile::pRes offset should match EEex docs");
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

  expect_eq(sizeof(PVRTextureHeaderV3), std::size_t{0x34},
            "PVRTextureHeaderV3 should match EEex docs");
  expect_eq(sizeof(ResFixedHeader_st), std::size_t{0x14},
            "ResFixedHeader_st should match EEex docs");
  expect_eq(sizeof(TisFileHeader), std::size_t{0x18},
            "TisFileHeader should carry the locally validated tile-dimension field");
  expect_eq(sizeof(WED_WedHeader_st), std::size_t{0x2C}, "WED_WedHeader_st should match EEex docs");
  expect_eq(sizeof(WED_LayerHeader_st), std::size_t{0x18},
            "WED_LayerHeader_st should match EEex docs");
  expect_eq(sizeof(WED_PolyHeader_st), std::size_t{0x14},
            "WED_PolyHeader_st should match EEex docs");
  expect_eq(sizeof(WED_PolyList_st), std::size_t{0x12}, "WED_PolyList_st should match EEex docs");
  expect_eq(sizeof(WED_PolyPoint_st), std::size_t{0x4}, "WED_PolyPoint_st should match EEex docs");
  expect_eq(sizeof(WED_ScreenSectionList), std::size_t{0x4},
            "WED_ScreenSectionList should match EEex docs");
  expect_eq(sizeof(WED_TileData_st), std::size_t{0xA}, "WED_TileData_st should match EEex docs");
  expect_eq(sizeof(WED_TiledObject_st), std::size_t{0x1A},
            "WED_TiledObject_st should match EEex docs");
  expect_eq(sizeof(bamHeader_st), std::size_t{0x18}, "bamHeader_st should match EEex docs");
  expect_eq(sizeof(BAMHEADERV2), std::size_t{0x20}, "BAMHEADERV2 should match EEex docs");
  expect_eq(sizeof(frame), std::size_t{0x18}, "frame should match EEex docs");
  expect_eq(sizeof(frameTableEntry_st), std::size_t{0xC},
            "frameTableEntry_st should match EEex docs");
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
  const auto find_field = [](const auto& fields, std::string_view name) -> const FieldDesc* {
    for (const auto& field : fields) {
      if (field.name == name) {
        return &field;
      }
    }
    return nullptr;
  };

  expect_eq(CInfGameLayout.size, std::uint32_t{0x97F8},
            "CInfGame doc layout size should match EEex docs");
  expect_eq(CInfGameLayout.fieldCount, std::uint32_t{128},
            "CInfGame doc layout should expose every EEex field row");
  expect_eq(CInfinityLayout.size, std::uint32_t{0x498},
            "CInfinity doc layout size should match EEex docs");
  expect_eq(CInfinityLayout.fieldCount, std::uint32_t{90},
            "CInfinity doc layout should expose every EEex field row");
  expect_eq(CGameAreaLayout.size, std::uint32_t{0x1120},
            "CGameArea doc layout size should match EEex docs");
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

  const auto* animationShaderField = find_field(CGameAnimationTypeFields, "m_bUseSpriteShader");
  expect_true(animationShaderField != nullptr,
              "CGameAnimationType should expose the sprite-shader field from EEex docs");
  if (animationShaderField) {
    expect_eq(animationShaderField->offset, std::uint32_t{0x20},
              "CGameAnimationType::m_bUseSpriteShader offset should match EEex docs");
  }

  const auto* optionShaderField = find_field(CGameOptionsFields, "m_bUseSpriteShader");
  expect_true(optionShaderField != nullptr,
              "CGameOptions should expose the sprite-shader option from EEex docs");
  if (optionShaderField) {
    expect_eq(optionShaderField->offset, std::uint32_t{0x238},
              "CGameOptions::m_bUseSpriteShader offset should match EEex docs");
  }

  const auto* areaInfinityField = find_field(CGameAreaFields, "m_cInfinity");
  expect_true(areaInfinityField != nullptr,
              "CGameArea should expose the embedded CInfinity field from EEex docs");
  if (areaInfinityField) {
    expect_eq(areaInfinityField->offset, std::uint32_t{0x5C8},
              "CGameArea::m_cInfinity offset should match EEex docs");
  }

  const auto* areaTiledObjectField = find_field(CGameAreaFields, "m_lTiledObjects");
  expect_true(areaTiledObjectField != nullptr,
              "CGameArea should expose the tiled-object list from EEex docs");
  if (areaTiledObjectField) {
    expect_eq(areaTiledObjectField->offset, std::uint32_t{0xED0},
              "CGameArea::m_lTiledObjects offset should match EEex docs");
  }

  const auto* visibleAreaField = find_field(CInfGameFields, "m_visibleArea");
  expect_true(visibleAreaField != nullptr,
              "CInfGame should expose the visible-area selector from EEex docs");
  if (visibleAreaField) {
    expect_eq(visibleAreaField->offset, std::uint32_t{0x6590},
              "CInfGame::m_visibleArea offset should match EEex docs");
  }

  const auto* gameAreasField = find_field(CInfGameFields, "m_gameAreas");
  expect_true(gameAreasField != nullptr,
              "CInfGame should expose the loaded area table from EEex docs");
  if (gameAreasField) {
    expect_eq(gameAreasField->offset, std::uint32_t{0x6598},
              "CInfGame::m_gameAreas offset should match EEex docs");
  }

  const auto* masterAreaField = find_field(CInfGameFields, "m_pGameAreaMaster");
  expect_true(masterAreaField != nullptr,
              "CInfGame should expose the master area pointer from EEex docs");
  if (masterAreaField) {
    expect_eq(masterAreaField->offset, std::uint32_t{0x65F8},
              "CInfGame::m_pGameAreaMaster offset should match EEex docs");
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
  }

  iee::core::EngineConfig cfg{};
  expect_true(iee::core::ConfigManager::load(tempPath, cfg),
              "ConfigManager::load should parse a valid INI");
  expect_true(cfg.enableVerboseLogging, "Verbose logging flag should parse");
  expect_true(!cfg.enableAnisotropicFiltering, "Rendering bool should parse");
  expect_eq(cfg.maxAnisotropy, 4.0f, "Floating-point values should parse");
  expect_eq(cfg.lodBias, -0.5f, "Negative float values should parse");

  std::error_code ec;
  std::filesystem::remove(tempPath, ec);
}

void test_config_numeric_bounds() {
  const auto tempPath = std::filesystem::current_path() / "InfinityEngine-Enhancer-bounds-test.ini";
  {
    std::ofstream out(tempPath, std::ios::trunc);
    out << "[Rendering]\n";
    out << "MaxAnisotropy = 1000\n";
    out << "LODBias = nan\n";
    out << "LODBias = 0.5junk\n";
  }

  iee::core::EngineConfig cfg{};
  iee::core::ConfigLoadDiagnostics diagnostics{};
  expect_true(iee::core::ConfigManager::load(tempPath, cfg, &diagnostics),
              "ConfigManager::load should accept and normalize numeric input");
  expect_eq(cfg.maxAnisotropy, 64.0f, "Anisotropy should be clamped to a safe bound");
  expect_eq(cfg.lodBias, -0.25f, "Non-finite LOD bias should use the default");
  expect_eq(diagnostics.invalidValues, std::size_t{2},
            "Invalid numeric values should be reported to the bootstrap logger");

  std::error_code error;
  std::filesystem::remove(tempPath, error);
}

void test_config_reports_malformed_values() {
  const auto tempPath =
      std::filesystem::current_path() / "InfinityEngine-Enhancer-invalid-test.ini";
  {
    std::ofstream out(tempPath, std::ios::trunc);
    out << "[Core]\n";
    out << "VerboseLogs = perhaps\n";
    out << "this line has no equals sign\n";
  }

  iee::core::EngineConfig cfg{};
  iee::core::ConfigLoadDiagnostics diagnostics{};
  expect_true(iee::core::ConfigManager::load(tempPath, cfg, &diagnostics),
              "Readable config files should keep valid/default values");
  expect_true(!cfg.enableVerboseLogging, "Invalid bool should retain its safe default");
  expect_eq(diagnostics.invalidValues, std::size_t{1},
            "Invalid bool should be counted for post-init diagnostics");
  expect_eq(diagnostics.malformedLines, std::size_t{1},
            "Malformed lines should be counted for post-init diagnostics");

  std::error_code error;
  std::filesystem::remove(tempPath, error);
}

void test_config_shader_override_defaults() {
  iee::core::EngineConfig cfg{};
  expect_true(!cfg.dumpEngineShaders, "shader dump defaults off");
  expect_true(!cfg.enableDebugHotkeys, "hotkeys default off");
  expect_true(cfg.enableWaterEffect, "water effect defaults ON");
  expect_true(!cfg.enablePerformanceLogging, "performance logs default off");
}

void test_performance_sample_summary() {
  iee::core::PerformanceSamples<4> samples;
  samples.add(4.0);
  samples.add(1.0);
  samples.add(3.0);
  samples.add(2.0);
  samples.add(99.0);

  const auto summary = samples.summarize();
  expect_eq(summary.count, std::size_t{4}, "Performance samples should stay bounded");
  expect_eq(summary.dropped, std::size_t{1}, "Overflow samples should be reported");
  expect_eq(summary.average, 2.5, "Performance sample average should be exact");
  expect_eq(summary.percentile95, 4.0, "Nearest-rank index should report the bounded p95");
  expect_eq(summary.maximum, 4.0, "Performance sample maximum should be retained");

  samples.reset();
  expect_eq(samples.summarize().count, std::size_t{0}, "Reset should clear the sample window");
}

void test_config_shader_override_roundtrip() {
  const auto tempPath = std::filesystem::current_path() / "InfinityEngine-Enhancer-shader-test.ini";
  {
    iee::core::EngineConfig orig{};
    orig.dumpEngineShaders = false;
    orig.enableDebugHotkeys = true;
    orig.enableWaterEffect = false;
    orig.enablePerformanceLogging = true;

    expect_true(iee::core::ConfigManager::save(tempPath, orig),
                "ConfigManager::save should succeed");
  }

  iee::core::EngineConfig loaded{};
  expect_true(iee::core::ConfigManager::load(tempPath, loaded),
              "ConfigManager::load should parse shader config");
  expect_true(!loaded.dumpEngineShaders, "dumpEngineShaders should round-trip as false");
  expect_true(loaded.enableDebugHotkeys, "enableDebugHotkeys should round-trip as true");
  expect_true(!loaded.enableWaterEffect, "enableWaterEffect should round-trip as false");
  expect_true(loaded.enablePerformanceLogging,
              "enablePerformanceLogging should round-trip as true");

  std::error_code ec;
  std::filesystem::remove(tempPath, ec);
}

void write_bytes(std::vector<std::byte>& buffer, std::size_t offset, const void* data,
                 std::size_t size) {
  if (offset + size > buffer.size()) {
    buffer.resize(offset + size);
  }
  std::memcpy(buffer.data() + offset, data, size);
}

constexpr std::uint32_t dds_four_cc(char a, char b, char c, char d) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

void write_u32(std::vector<std::byte>& buffer, std::size_t offset, std::uint32_t value) {
  if (offset + sizeof(value) > buffer.size()) buffer.resize(offset + sizeof(value));
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    buffer[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xFF);
  }
}

std::vector<std::byte> make_legacy_dds(std::uint32_t formatCode, std::uint32_t width,
                                       std::uint32_t height, std::uint32_t mipCount,
                                       std::size_t payloadBytes,
                                       std::uint32_t additionalPixelFormatFlags = 0) {
  constexpr std::size_t headerBytes = 128;
  std::vector<std::byte> bytes(headerBytes + payloadBytes);
  constexpr char magic[] = "DDS ";
  write_bytes(bytes, 0, magic, 4);
  write_u32(bytes, 4, 124);  // DDS_HEADER::dwSize
  write_u32(bytes, 12, height);
  write_u32(bytes, 16, width);
  write_u32(bytes, 28, mipCount);
  write_u32(bytes, 76, 32);                                // DDS_PIXELFORMAT::dwSize
  write_u32(bytes, 80, 0x4 | additionalPixelFormatFlags);  // DDPF_FOURCC
  write_u32(bytes, 84, formatCode);
  for (std::size_t index = 0; index < payloadBytes; ++index) {
    bytes[headerBytes + index] = static_cast<std::byte>(index & 0xFF);
  }
  return bytes;
}

std::vector<std::byte> make_dx10_dds(std::uint32_t dxgiFormat, std::uint32_t width,
                                     std::uint32_t height, std::uint32_t mipCount,
                                     std::size_t payloadBytes) {
  constexpr std::size_t headerBytes = 148;
  auto bytes =
      make_legacy_dds(dds_four_cc('D', 'X', '1', '0'), width, height, mipCount, payloadBytes + 20);
  bytes.resize(headerBytes + payloadBytes);
  write_u32(bytes, 128, dxgiFormat);
  write_u32(bytes, 132, 3);  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
  write_u32(bytes, 136, 0);  // miscFlag
  write_u32(bytes, 140, 1);  // arraySize
  write_u32(bytes, 144, 0);  // miscFlags2
  for (std::size_t index = 0; index < payloadBytes; ++index) {
    bytes[headerBytes + index] = static_cast<std::byte>((index + 17) & 0xFF);
  }
  return bytes;
}

void test_parse_dds_legacy_formats_and_mips() {
  using namespace iee::game;

  auto bytes = make_legacy_dds(dds_four_cc('D', 'X', 'T', '1'), 8, 8, 4, 56);
  DdsTexture texture;
  std::string error;
  expect_true(parse_dds_texture(bytes, texture, error),
              "Legacy BC1 DDS with a full mip chain should parse");
  if (texture.empty()) return;

  expect_true(error.empty(), "Successful DDS parsing should clear the error string");
  expect_true(texture.format == DdsBlockFormat::Bc1RgbUnorm,
              "Legacy DXT1 without alpha pixels should map to BC1 RGB");
  expect_eq(texture.width, std::uint32_t{8}, "DDS width should be preserved");
  expect_eq(texture.height, std::uint32_t{8}, "DDS height should be preserved");
  expect_eq(texture.mipLevels.size(), std::size_t{4}, "All declared DDS mips should be exposed");
  expect_eq(texture.payload.size(), std::size_t{56}, "Only the required mip payload should remain");
  const std::array<std::uint32_t, 4> dimensions{8, 4, 2, 1};
  const std::array<std::size_t, 4> offsets{0, 32, 40, 48};
  for (std::size_t index = 0; index < texture.mipLevels.size(); ++index) {
    expect_eq(texture.mipLevels[index].width, dimensions[index],
              "DDS mip width should halve to one");
    expect_eq(texture.mipLevels[index].height, dimensions[index],
              "DDS mip height should halve to one");
    expect_eq(texture.mipLevels[index].dataOffset, offsets[index],
              "DDS mip offsets should follow block-compressed sizes");
    expect_eq(texture.mipLevels[index].dataSize, index == 0 ? std::size_t{32} : std::size_t{8},
              "BC1 mip sizes should use 8-byte 4x4 blocks");
  }

  bytes = make_legacy_dds(dds_four_cc('D', 'X', 'T', '1'), 4, 4, 1, 8, 0x1);
  expect_true(parse_dds_texture(bytes, texture, error), "Legacy BC1 alpha DDS should parse");
  expect_true(texture.format == DdsBlockFormat::Bc1RgbaUnorm,
              "Legacy DXT1 with alpha pixels should map to BC1 RGBA");

  bytes = make_legacy_dds(dds_four_cc('D', 'X', 'T', '5'), 4, 4, 1, 16);
  expect_true(parse_dds_texture(bytes, texture, error), "Legacy BC3 DDS should parse");
  expect_true(texture.format == DdsBlockFormat::Bc3RgbaUnorm, "Legacy DXT5 should map to BC3 RGBA");

  bytes = make_legacy_dds(dds_four_cc('A', 'T', 'I', '2'), 4, 4, 1, 16);
  expect_true(parse_dds_texture(bytes, texture, error), "Legacy BC5 DDS should parse");
  expect_true(texture.format == DdsBlockFormat::Bc5RgUnorm, "Legacy ATI2 should map to BC5 RG");

  bytes = make_legacy_dds(dds_four_cc('B', 'C', '5', 'U'), 4, 4, 1, 16);
  expect_true(parse_dds_texture(bytes, texture, error), "Legacy BC5U DDS should parse");
  expect_true(texture.format == DdsBlockFormat::Bc5RgUnorm, "Legacy BC5U should map to BC5 RG");
}

void test_parse_dds_dx10_formats() {
  using namespace iee::game;

  struct FormatCase {
    std::uint32_t dxgiFormat;
    DdsBlockFormat expected;
    std::size_t blockBytes;
  };
  constexpr std::array<FormatCase, 7> cases{{
      {71, DdsBlockFormat::Bc1RgbaUnorm, 8},
      {72, DdsBlockFormat::Bc1RgbaSrgb, 8},
      {77, DdsBlockFormat::Bc3RgbaUnorm, 16},
      {78, DdsBlockFormat::Bc3RgbaSrgb, 16},
      {83, DdsBlockFormat::Bc5RgUnorm, 16},
      {98, DdsBlockFormat::Bc7RgbaUnorm, 16},
      {99, DdsBlockFormat::Bc7RgbaSrgb, 16},
  }};

  for (const auto& formatCase : cases) {
    auto bytes = make_dx10_dds(formatCase.dxgiFormat, 4, 4, 1, formatCase.blockBytes);
    DdsTexture texture;
    std::string error;
    expect_true(parse_dds_texture(bytes, texture, error),
                "Supported DX10 block-compressed DDS should parse");
    expect_true(texture.format == formatCase.expected,
                "DXGI compression format should map to the expected runtime format");
    expect_eq(texture.payload.size(), formatCase.blockBytes,
              "DX10 DDS should expose exactly one compressed block");
  }
}

void test_parse_dds_rejects_unsupported_or_malformed_input() {
  using namespace iee::game;

  DdsTexture texture;
  std::string error;
  auto bytes = make_legacy_dds(dds_four_cc('D', 'X', 'T', '1'), 8, 8, 1, 31);
  expect_true(!parse_dds_texture(bytes, texture, error),
              "Truncated BC1 payload should fail closed");
  expect_true(texture.empty() && !error.empty(),
              "Rejected DDS input should clear output and explain the failure");

  bytes = make_legacy_dds(dds_four_cc('D', 'X', 'T', '3'), 4, 4, 1, 16);
  expect_true(!parse_dds_texture(bytes, texture, error), "Unsupported BC2/DXT3 should be rejected");

  bytes = make_legacy_dds(dds_four_cc('D', 'X', 'T', '1'), 4, 4, 1, 8);
  write_u32(bytes, 112, 0x200);  // DDSCAPS2_CUBEMAP
  expect_true(!parse_dds_texture(bytes, texture, error), "Legacy cubemap DDS should be rejected");

  bytes = make_dx10_dds(98, 4, 4, 1, 16);
  write_u32(bytes, 140, 2);
  expect_true(!parse_dds_texture(bytes, texture, error), "DX10 texture arrays should be rejected");

  bytes = make_legacy_dds(dds_four_cc('D', 'X', 'T', '1'), 4, 4, 4, 32);
  expect_true(!parse_dds_texture(bytes, texture, error),
              "A DDS with more mips than its dimensions allow should be rejected");

  bytes = make_dx10_dds(95, 4, 4, 1, 16);  // BC6H_UF16
  expect_true(!parse_dds_texture(bytes, texture, error),
              "Unsupported DXGI compression formats should be rejected");
}

void test_load_dds_texture_file_wrapper() {
  const auto tempPath = std::filesystem::current_path() / "InfinityEngine-Enhancer-dds-test.dds";
  const auto bytes = make_dx10_dds(98, 4, 4, 1, 16);
  {
    std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    expect_true(static_cast<bool>(file), "Synthetic DDS fixture should be written completely");
  }

  iee::game::DdsTexture texture;
  std::string error;
  expect_true(iee::game::load_dds_texture(tempPath, texture, error),
              "DDS file wrapper should load a valid bounded file");
  expect_true(texture.format == iee::game::DdsBlockFormat::Bc7RgbaUnorm,
              "DDS file wrapper should preserve the parsed format");

  std::error_code removeError;
  std::filesystem::remove(tempPath, removeError);
  expect_true(!removeError, "Synthetic DDS fixture should be removed after the test");

  expect_true(!iee::game::load_dds_texture(tempPath, texture, error),
              "DDS file wrapper should reject a missing file");
  expect_true(texture.empty() && !error.empty(),
              "Missing DDS files should clear output and report an error");
}

void test_parse_loaded_wed() {
  using namespace iee::game;

  constexpr std::size_t headerOffset = 0x0;
  constexpr std::size_t layerOffset = sizeof(WED_WedHeader_st);
  constexpr std::size_t tilemapOffset = layerOffset + 2 * sizeof(WED_LayerHeader_st);
  constexpr std::size_t liquidTilemapOffset = tilemapOffset + 4 * sizeof(WED_TileData_st);
  constexpr std::size_t liquidLookupOffset = liquidTilemapOffset + 4 * sizeof(WED_TileData_st);

  std::vector<std::byte> bytes(liquidLookupOffset + 4 * sizeof(std::uint16_t));

  WED_WedHeader_st header{};
  header.nFileType = 0x20444557;
  header.nFileVersion = 0x332E3156;
  header.nLayers = 2;
  header.nOffsetToLayerHeaders = static_cast<std::uint32_t>(layerOffset);
  write_bytes(bytes, headerOffset, &header, sizeof(header));

  WED_LayerHeader_st baseLayer{};
  baseLayer.nTilesAcross = 2;
  baseLayer.nTilesDown = 2;
  baseLayer.rrTileSet = {'A', 'R', '0', '0', '0', '1', '0', '0'};
  baseLayer.nNumUniqueTiles = 4;
  baseLayer.nOffsetToTileData = static_cast<std::uint32_t>(tilemapOffset);
  write_bytes(bytes, layerOffset, &baseLayer, sizeof(baseLayer));

  WED_LayerHeader_st liquidLayer{};
  liquidLayer.nTilesAcross = 2;
  liquidLayer.nTilesDown = 2;
  liquidLayer.rrTileSet = {'W', 'T', 'W', 'A', 'V', 'E', '0', '1'};
  liquidLayer.nNumUniqueTiles = 4;
  liquidLayer.nOffsetToTileData = static_cast<std::uint32_t>(liquidTilemapOffset);
  liquidLayer.nOffsetToTileList = static_cast<std::uint32_t>(liquidLookupOffset);
  write_bytes(bytes, layerOffset + sizeof(WED_LayerHeader_st), &liquidLayer, sizeof(liquidLayer));

  std::array<WED_TileData_st, 4> tileData{};
  tileData[0].bFlags = 0x02;
  tileData[2].bFlags = 0x02;
  write_bytes(bytes, tilemapOffset, tileData.data(), sizeof(tileData));

  std::array<WED_TileData_st, 4> liquidTileData{};
  liquidTileData[0].nStartingTile = 3;
  liquidTileData[1].nStartingTile = 0;
  liquidTileData[2].nStartingTile = 2;
  liquidTileData[3].nStartingTile = 500;  // lookup entry out of bounds
  write_bytes(bytes, liquidTilemapOffset, liquidTileData.data(), sizeof(liquidTileData));

  const std::array<std::uint16_t, 4> liquidTileLookup{21, 22, 23, 24};
  write_bytes(bytes, liquidLookupOffset, liquidTileLookup.data(), sizeof(liquidTileLookup));

  CRes resource{};
  resource.pData = bytes.data();
  resource.nSize = static_cast<std::uint32_t>(bytes.size());
  resource.bLoaded = true;

  char areaResref[8] = {'A', 'R', '0', '0', '0', '1', '\0', '\0'};
  resource.resref = areaResref;

  WedAreaInfo wed{};
  expect_true(parse_loaded_wed(resource, wed), "Loaded WED blob should parse");
  expect_eq(wed.overlayCount, std::uint32_t{2}, "WED parser should expose overlay count");
  expect_eq(wed.baseWidth, std::uint16_t{2}, "WED parser should expose base width");
  expect_eq(wed.baseHeight, std::uint16_t{2}, "WED parser should expose base height");
  expect_true(wed.overlays.size() == 2, "WED parser should keep both overlays");
  expect_true(wed.overlays[1].liquidMode == TileLiquidMode::Water,
              "Liquid tileset classifier should mark water overlays");
  expect_eq(wed.overlays[1].coverageCells, std::uint32_t{2},
            "Liquid overlay coverage should count flagged base cells");
  expect_eq(liquid_overlay_mask(wed), std::uint8_t{0x02},
            "Liquid overlay mask should expose overlay bit");

  expect_true(wed.overlays[0].tintTileCandidates.empty(),
              "Base overlay should not carry tint tile candidates");
  expect_eq(wed.overlays[1].tintTileCandidates.size(), std::size_t{3},
            "Liquid overlay should carry bounded unique tint candidates");
  expect_eq(wed.overlays[1].tintTileCandidates[0], std::uint16_t{24},
            "Cell 0 tile index should resolve through the tile-index lookup");
  expect_eq(wed.overlays[1].tintTileCandidates[1], std::uint16_t{21},
            "Cell 1 tile index should resolve through the tile-index lookup");
  expect_eq(wed.overlays[1].tintTileCandidates[2], std::uint16_t{23},
            "Cell 2 tile index should resolve through the tile-index lookup");

  auto tooManyLayers = bytes;
  auto invalidHeader = header;
  invalidHeader.nLayers = 9;
  write_bytes(tooManyLayers, headerOffset, &invalidHeader, sizeof(invalidHeader));
  resource.pData = tooManyLayers.data();
  resource.nSize = static_cast<std::uint32_t>(tooManyLayers.size());
  expect_true(!parse_loaded_wed(resource, wed),
              "WED files with more than eight layers should fail closed");
  expect_true(wed.empty(), "A rejected WED should not leave partially parsed state");

  auto excessiveDimensions = bytes;
  auto invalidBaseLayer = baseLayer;
  invalidBaseLayer.nTilesAcross = 0xFFFF;
  invalidBaseLayer.nTilesDown = 0xFFFF;
  write_bytes(excessiveDimensions, layerOffset, &invalidBaseLayer, sizeof(invalidBaseLayer));
  resource.pData = excessiveDimensions.data();
  resource.nSize = static_cast<std::uint32_t>(excessiveDimensions.size());
  expect_true(!parse_loaded_wed(resource, wed),
              "Excessive WED dimensions should be rejected before allocation");

  resource.pData = bytes.data();
  resource.nSize = static_cast<std::uint32_t>(tilemapOffset + 3 * sizeof(WED_TileData_st));
  expect_true(!parse_loaded_wed(resource, wed),
              "A truncated base tilemap should fail instead of returning partial data");
}

void test_wed_tint_candidates_are_bounded() {
  using namespace iee::game;

  constexpr std::size_t cellCount = kMaxTintTileCandidatesPerOverlay + 1;
  constexpr std::size_t layerOffset = sizeof(WED_WedHeader_st);
  constexpr std::size_t baseTilemapOffset = layerOffset + 2 * sizeof(WED_LayerHeader_st);
  constexpr std::size_t overlayTilemapOffset =
      baseTilemapOffset + cellCount * sizeof(WED_TileData_st);
  constexpr std::size_t lookupOffset = overlayTilemapOffset + cellCount * sizeof(WED_TileData_st);
  std::vector<std::byte> bytes(lookupOffset + cellCount * sizeof(std::uint16_t));

  WED_WedHeader_st header{};
  header.nFileType = 0x20444557;
  header.nFileVersion = 0x332E3156;
  header.nLayers = 2;
  header.nOffsetToLayerHeaders = static_cast<std::uint32_t>(layerOffset);
  write_bytes(bytes, 0, &header, sizeof(header));

  WED_LayerHeader_st baseLayer{};
  baseLayer.nTilesAcross = static_cast<std::uint16_t>(cellCount);
  baseLayer.nTilesDown = 1;
  baseLayer.nOffsetToTileData = static_cast<std::uint32_t>(baseTilemapOffset);
  write_bytes(bytes, layerOffset, &baseLayer, sizeof(baseLayer));

  WED_LayerHeader_st overlayLayer{};
  overlayLayer.nTilesAcross = static_cast<std::uint16_t>(cellCount);
  overlayLayer.nTilesDown = 1;
  overlayLayer.rrTileSet = {'W', 'T', 'W', 'A', 'V', 'E', '0', '1'};
  overlayLayer.nOffsetToTileData = static_cast<std::uint32_t>(overlayTilemapOffset);
  overlayLayer.nOffsetToTileList = static_cast<std::uint32_t>(lookupOffset);
  write_bytes(bytes, layerOffset + sizeof(WED_LayerHeader_st), &overlayLayer, sizeof(overlayLayer));

  std::vector<WED_TileData_st> baseTiles(cellCount);
  std::vector<WED_TileData_st> overlayTiles(cellCount);
  std::vector<std::uint16_t> lookup(cellCount);
  for (std::size_t index = 0; index < cellCount; ++index) {
    baseTiles[index].bFlags = 0x02;
    overlayTiles[index].nStartingTile = static_cast<std::uint16_t>(index);
    lookup[index] = static_cast<std::uint16_t>(index);
  }
  write_bytes(bytes, baseTilemapOffset, baseTiles.data(), baseTiles.size() * sizeof(baseTiles[0]));
  write_bytes(bytes, overlayTilemapOffset, overlayTiles.data(),
              overlayTiles.size() * sizeof(overlayTiles[0]));
  write_bytes(bytes, lookupOffset, lookup.data(), lookup.size() * sizeof(lookup[0]));

  CRes resource{};
  resource.pData = bytes.data();
  resource.nSize = static_cast<std::uint32_t>(bytes.size());
  resource.bLoaded = true;
  WedAreaInfo wed{};
  expect_true(parse_loaded_wed(resource, wed), "Bounded-candidate WED fixture should parse");
  if (wed.overlays.size() > 1) {
    expect_eq(wed.overlays[1].tintTileCandidates.size(), kMaxTintTileCandidatesPerOverlay,
              "Liquid tint candidates must stop at the fixed memory bound");
  }
}

void test_decode_palette_tile_alpha() {
  using namespace iee::game;

  std::vector<std::uint8_t> tile(kPaletteTileBytes, 0);

  // BGRA palette entries. Entry 0 is transparent by index regardless of
  // color; entry 1 is the green key; entries 2 and 3 are opaque colors,
  // with entry 3 deliberately near-green.
  const auto setEntry = [&](std::size_t i, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    tile[i * 4 + 0] = b;
    tile[i * 4 + 1] = g;
    tile[i * 4 + 2] = r;
    tile[i * 4 + 3] = 255;
  };
  setEntry(0, 255, 0, 0);
  setEntry(1, 0, 255, 0);
  setEntry(2, 255, 255, 255);
  setEntry(3, 0, 255, 1);

  std::uint8_t* indices = tile.data() + 1024;
  std::fill(indices, indices + kTilePixels * kTilePixels, std::uint8_t{2});
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 3;

  const auto alpha = decode_palette_tile_alpha(tile.data(), tile.size());
  expect_true(alpha.has_value(), "Full-size palette tile should decode");
  expect_eq(alpha->opaque[0], std::uint8_t{0}, "Palette index 0 should be transparent");
  expect_eq(alpha->opaque[1], std::uint8_t{0}, "Green-key palette entries should be transparent");
  expect_eq(alpha->opaque[2], std::uint8_t{1}, "Near-green palette entries should stay opaque");
  expect_eq(alpha->opaque[3], std::uint8_t{1}, "Ordinary palette entries should be opaque");
  expect_eq(alpha->opaque[kTilePixels * kTilePixels - 1], std::uint8_t{1},
            "Last pixel should decode like any other");

  expect_true(!decode_palette_tile_alpha(tile.data(), kPaletteTileBytes - 1).has_value(),
              "Undersized buffers should not decode as palette tiles");
  expect_true(!decode_palette_tile_alpha(nullptr, kPaletteTileBytes).has_value(),
              "Null buffers should not decode");

  // Average opaque color in linear light: half pure red, half pure blue ->
  // (0.5, 0, 0.5).
  std::vector<std::uint8_t> colorTile(kPaletteTileBytes, 0);
  colorTile[2 * 4 + 2] = 255;  // entry 2: red (BGRA)
  colorTile[3 * 4 + 0] = 255;  // entry 3: blue
  std::uint8_t* colorIndices = colorTile.data() + 1024;
  for (int i = 0; i < kTilePixels * kTilePixels; ++i) {
    colorIndices[i] = (i < kTilePixels * kTilePixels / 2) ? 2 : 3;
  }
  const auto avg = palette_tile_average_color(colorTile.data(), colorTile.size());
  expect_true(avg.has_value(), "Opaque tile should yield an average color");
  if (avg) {
    expect_true(avg->linearRgb[0] == 0.5f && avg->linearRgb[1] == 0.0f && avg->linearRgb[2] == 0.5f,
                "Average color should be the exact opaque-pixel mean");
    expect_eq(avg->opaquePixelCount, std::size_t{kTilePixels * kTilePixels},
              "Average should report its opaque-pixel weight");
  }
  // Mid-grey is encoded sRGB. A linear-light average must decode it before
  // feeding shader math rather than returning 128/255 (~0.502).
  std::vector<std::uint8_t> greyTile(kPaletteTileBytes, 0);
  greyTile[4] = 128;
  greyTile[5] = 128;
  greyTile[6] = 128;
  std::fill(greyTile.begin() + 1024, greyTile.end(), std::uint8_t{1});
  const auto grey = palette_tile_average_color(greyTile.data(), greyTile.size());
  expect_true(grey.has_value(), "Opaque grey tile should yield an average color");
  if (grey) {
    expect_true(std::abs(grey->linearRgb[0] - 0.215861f) < 0.00001f &&
                    std::abs(grey->linearRgb[1] - 0.215861f) < 0.00001f &&
                    std::abs(grey->linearRgb[2] - 0.215861f) < 0.00001f,
                "Palette averages should be decoded into linear light");
  }

  // Fully transparent tile (all indices 0) -> no average color.
  std::vector<std::uint8_t> emptyTile(kPaletteTileBytes, 0);
  emptyTile[1] = 255;  // entry 0 green just to vary the palette
  expect_true(!palette_tile_average_color(emptyTile.data(), emptyTile.size()).has_value(),
              "Fully transparent tiles should yield no average color");
}

void test_tile_table_detection_ignores_garbage_steps() {
  using namespace iee::game;

  TileInfo tileInfo{};
  std::array<PVRZTileEntry, 3> table{{
      {17, 2384508, 1854076},
      {17, 799324, 1333884},
      {17, 3420796, 805484},
  }};
  tileInfo.table = table.data();
  tileInfo.tileCount = static_cast<std::uint32_t>(table.size());

  const auto detection = infer_scale_from_tile_table(tileInfo);
  expect_true(!detection.has_value(),
              "Tile-table detection should ignore garbage UV steps instead of treating them as "
              "deterministic scale");
}

void test_tile_table_detection_uses_coordinate_deltas() {
  using namespace iee::game;

  TileInfo upscaledInfo{};
  std::array<PVRZTileEntry, 4> upscaledTable{{
      {7, 128, 64},
      {7, 384, 64},
      {7, 128, 320},
      {8, 129, 65},  // A different page must not affect the grid step.
  }};
  upscaledInfo.table = upscaledTable.data();
  upscaledInfo.tileCount = static_cast<std::uint32_t>(upscaledTable.size());
  const auto upscaled = infer_scale_from_tile_table(upscaledInfo);
  expect_true(upscaled.has_value(), "Translated 4x atlas coordinates should resolve");
  if (upscaled) {
    expect_eq(upscaled->scaleFactor, 4, "A 256px coordinate grid should resolve to 4x");
  }

  TileInfo standardInfo{};
  std::array<PVRZTileEntry, 3> standardTable{{
      {2, 192, 128},
      {2, 256, 128},
      {2, 192, 192},
  }};
  standardInfo.table = standardTable.data();
  standardInfo.tileCount = static_cast<std::uint32_t>(standardTable.size());
  const auto standard = infer_scale_from_tile_table(standardInfo);
  expect_true(standard.has_value(), "Translated standard atlas coordinates should resolve");
  if (standard) {
    expect_eq(standard->scaleFactor, 1, "A 64px coordinate grid should resolve to standard");
  }
}

void test_manifest_infgame_offsets() {
  const auto& m = iee::game::current_manifest();
  expect_eq(m.offsets.infGameVisibleArea, std::uintptr_t{0x6590}, "visible area offset");
  expect_eq(m.offsets.infGameAreas, std::uintptr_t{0x6598}, "areas array offset");
  expect_eq(m.offsets.infGameAreaMaster, std::uintptr_t{0x65F8}, "master area offset");
  expect_true(m.validate(), "manifest still validates");
}

iee::game::TileInfo make_tile_info(std::uint32_t tileDimension, int texId, int u, int v,
                                   bool includeHeader = true, bool* outLinearFlag = nullptr) {
  const auto& manifest = iee::game::current_manifest();

  static std::vector<std::byte> vidTileStorage;
  static std::vector<std::byte> tilesetStorage;
  static iee::game::TisFileHeader header;
  static std::array<iee::game::PVRZTileEntry, 2> table;
  static iee::game::CResTile resource;

  vidTileStorage.assign(manifest.offsets.vidTileResource + sizeof(iee::game::CResTile*),
                        std::byte{0});
  tilesetStorage.assign(manifest.offsets.tisLinearTilesFlag + sizeof(std::int32_t), std::byte{0});

  auto* tileset = reinterpret_cast<iee::game::CResTileSet*>(tilesetStorage.data());
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
    std::memcpy(tilesetStorage.data() + manifest.offsets.tisLinearTilesFlag, &linearValue,
                sizeof(linearValue));
  }

  auto* resourcePtr = &resource;
  std::memcpy(vidTileStorage.data() + manifest.offsets.vidTileResource, &resourcePtr,
              sizeof(resourcePtr));

  iee::game::TileInfo info{};
  const auto demand_passthrough = +[](void* p) -> void* { return p; };
  expect_true(iee::game::get_tile_info(vidTileStorage.data(), manifest, info, demand_passthrough),
              "get_tile_info should decode the synthetic CVidTile payload");
  return info;
}

void test_tis_header_dimension_decoding() {
  const auto& manifest = iee::game::current_manifest();
  auto tileInfo = make_tile_info(iee::game::TisTileDimensions::Upscaled4x, 15000,
                                 static_cast<int>(iee::game::TisTileDimensions::Upscaled4x), 0);

  const auto tileDimension = iee::game::get_tis_header_tile_dimension(tileInfo, manifest);
  expect_true(tileDimension.has_value(), "TIS header tile dimension should be readable");
  if (tileDimension) {
    expect_eq(*tileDimension, std::uint32_t{0x100},
              "4x tile dimension should decode from the header");
  }

  const auto detection = iee::game::detect_scale_from_tis_header(tileInfo, manifest);
  expect_true(detection.has_value(), "Known tile dimensions should resolve from the header");
  if (detection) {
    expect_eq(detection->scaleFactor, 4, "Header-based detection should map 0x100 to 4x scale");
    expect_true(detection->source == iee::game::ScaleDetectionSource::TisHeader,
                "Header-based detection should report the correct source");
  }
}

void test_supported_tile_dimensions_are_inferred_dynamically() {
  using namespace iee::game;

  for (const auto& [dimension, expectedScale] : std::array<std::pair<std::uint32_t, int>, 4>{{
           {TisTileDimensions::Standard, 1},
           {TisTileDimensions::Upscaled2x, 2},
           {TisTileDimensions::Upscaled4x, 4},
           {TisTileDimensions::Upscaled8x, 8},
       }}) {
    const auto scale = scale_factor_from_tile_dimension(dimension);
    expect_true(scale.has_value(), "supported power-of-two tile dimension should resolve");
    if (scale) expect_eq(*scale, expectedScale, "tile dimension should map to its scale factor");

    auto headerInfo = make_tile_info(dimension, 100, static_cast<int>(dimension), 0);
    const auto headerDetection = detect_scale_from_tis_header(headerInfo, current_manifest());
    expect_true(headerDetection.has_value(), "every supported header dimension should resolve");
    if (headerDetection) {
      expect_eq(headerDetection->scaleFactor, expectedScale,
                "header dimension should drive the same dynamic scale mapping");
    }
  }

  for (const auto dimension :
       {std::uint32_t{0}, std::uint32_t{96}, std::uint32_t{192}, std::uint32_t{1024}}) {
    expect_true(!scale_factor_from_tile_dimension(dimension).has_value(),
                "unsupported tile dimensions must fail closed");
  }

  TileInfo tableInfo{};
  std::array<PVRZTileEntry, 3> table{{
      {2, 64, 64},
      {2, 576, 64},
      {2, 64, 576},
  }};
  tableInfo.table = table.data();
  tableInfo.tileCount = static_cast<std::uint32_t>(table.size());
  const auto tableDetection = infer_scale_from_tile_table(tableInfo);
  expect_true(tableDetection.has_value(), "512px table grid should resolve dynamically");
  if (tableDetection) expect_eq(tableDetection->scaleFactor, 8, "512px grid should map to 8x");
}

void test_tis_table_entry_bounds() {
  using namespace iee::game;

  std::array<PVRZTileEntry, 1> table{{{3, 64, 128}}};
  TileInfo info{};
  info.table = table.data();
  info.tileCount = static_cast<std::uint32_t>(table.size());

  PVRZTileEntry entry{};
  expect_true(read_tis_tile_entry(info, 0, entry), "In-range TIS entries should be readable");
  expect_eq(entry.page, 3, "The requested TIS entry should be returned");
  expect_true(!read_tis_tile_entry(info, 1, entry),
              "TIS entry reads must reject indices at the count boundary");

  info.table = reinterpret_cast<const PVRZTileEntry*>((std::numeric_limits<std::uintptr_t>::max)() -
                                                      sizeof(PVRZTileEntry) + 1);
  info.tileCount = 2;
  expect_true(!read_tis_tile_entry(info, 1, entry),
              "TIS entry address arithmetic must reject integer overflow");

  auto overflowManifest = current_manifest();
  constexpr auto nearAddress = (std::numeric_limits<std::uintptr_t>::max)() - 3;
  overflowManifest.offsets.vidTileResource = 8;
  expect_true(!get_tile_info(reinterpret_cast<void*>(nearAddress), overflowManifest, info, nullptr),
              "CVidTile field address arithmetic must reject integer overflow");

  info.header = reinterpret_cast<const TisFileHeader*>(nearAddress);
  overflowManifest.offsets.tisHeaderTileDimension = 8;
  expect_true(!get_tis_header_tile_dimension(info, overflowManifest).has_value(),
              "TIS header field address arithmetic must reject integer overflow");

  overflowManifest.offsets.tisLinearTilesFlag = 8;
  expect_true(!get_tis_linear_tiles_flag(reinterpret_cast<const CResTileSet*>(nearAddress),
                                         overflowManifest),
              "TIS linear-flag address arithmetic must reject integer overflow");
}

void test_tileset_runtime_cache_is_bounded_and_resettable() {
  iee::features::TileRenderState state{};
  for (std::size_t i = 0; i < iee::features::TileRenderState::kMaxTilesetsPerArea; ++i) {
    const auto* tileset = reinterpret_cast<const iee::game::CResTileSet*>(i + 1);
    expect_true(state.find_or_add(tileset) != nullptr,
                "The bounded cache should accept its documented tileset capacity");
  }
  const auto* overflow = reinterpret_cast<const iee::game::CResTileSet*>(
      iee::features::TileRenderState::kMaxTilesetsPerArea + 1);
  expect_true(state.find_or_add(overflow) == nullptr,
              "The tileset cache must fail closed instead of growing without a bound");

  state.reset();
  expect_eq(state.tilesetCount, std::size_t{0}, "Area reset should release all cached tilesets");
  expect_true(state.find_or_add(overflow) != nullptr,
              "A reset cache should accept tilesets from the next area");
}

void test_scale_selection_precedence() {
  const auto& manifest = iee::game::current_manifest();

  auto standardInfo = make_tile_info(iee::game::TisTileDimensions::Standard, 20000,
                                     static_cast<int>(iee::game::TisTileDimensions::Standard), 0);
  const auto standardDetection = iee::game::detect_scale(standardInfo, 20000, manifest);
  expect_true(standardDetection.has_value(), "Header-first detection should produce a scale hint");
  if (standardDetection) {
    expect_eq(standardDetection->scaleFactor, 1,
              "Standard header values should win over heuristics");
    expect_true(standardDetection->source == iee::game::ScaleDetectionSource::TisHeader,
                "Standard header values should report TIS-header provenance");
  }

  auto tableOnlyInfo = make_tile_info(
      0x80, 20000, static_cast<int>(iee::game::TisTileDimensions::Upscaled4x), 0, false);
  const auto tableDetection = iee::game::detect_scale(tableOnlyInfo, 20000, manifest);
  expect_true(tableDetection.has_value(),
              "Table-derived detection should be used when the header is missing");
  if (tableDetection) {
    expect_eq(tableDetection->scaleFactor, 4, "Table-derived detection should detect 4x tiles");
    expect_true(tableDetection->source == iee::game::ScaleDetectionSource::TileTable,
                "Fallback should prefer deterministic table provenance over heuristics");
  }

  // Large raw UVs and high texture ids are not scale signals: with no header
  // and an unresolvable table, detection must fail closed (the render path
  // then samples and delegates the tileset as standard 1x).
  auto garbageInfo = make_tile_info(0x80, 20000, 4096, 4096);
  garbageInfo.header = nullptr;
  garbageInfo.tileCount = 1;
  const auto garbageDetection = iee::game::detect_scale(garbageInfo, 20000, manifest);
  expect_true(!garbageDetection.has_value(),
              "Garbage UV/texture-id input must not produce a scale detection");

  bool linearFlag = true;
  auto linearInfo = make_tile_info(iee::game::TisTileDimensions::Upscaled4x, 12000,
                                   static_cast<int>(iee::game::TisTileDimensions::Upscaled4x), 0,
                                   true, &linearFlag);
  expect_true(iee::game::get_tis_linear_tiles_flag(linearInfo.tileset, manifest),
              "The manifest linear-tiles offset should be readable from synthetic data");
}

void test_shader_name_extraction() {
  using iee::game::extract_shader_name;
  expect_eq(extract_shader_name("// fpSEAM.glsl\nuniform float uTcScale;", "fp"),
            std::string("fpSEAM"), "extracts fp name");
  expect_eq(extract_shader_name("// vpDraw.glsl\nvoid main(){}", "vp"), std::string("vpDraw"),
            "extracts vp name");
  expect_true(extract_shader_name("// vpDraw.glsl\n", "fp").empty(), "prefix filter rejects vp");
  expect_true(extract_shader_name("no comment here", "fp").empty(), "no name -> empty");
}

void test_interface_contract() {
  const std::string_view original =
      "// fpSEAM.glsl\nuniform sampler2D sTex;\nuniform float uTcScale;\nvarying vec2 vTc;\nvoid "
      "main(){}";
  const std::string_view good =
      "#version 460 compatibility\nuniform sampler2D sTex;\nuniform float uTcScale;\nvarying vec2 "
      "vTc;\nvoid main(){}";
  const std::string_view bad = "#version 460 compatibility\nuniform sampler2D sTex;\nvoid main(){}";
  expect_true(iee::game::check_interface_contract(original, good).ok, "matching interface passes");
  const auto failed = iee::game::check_interface_contract(original, bad);
  expect_true(!failed.ok, "missing identifiers fail");
  expect_eq(failed.missingIdentifiers.size(), std::size_t{2}, "uTcScale and vTc reported");
}

// Regression: sTex is a substring of sTex1; a replacement declaring only sTex1 must NOT
// satisfy the sTex contract check (multi-texture blend shaders use both samplers).
void test_interface_contract_token_boundary() {
  const std::string_view original =
      "// fpBLEND.glsl\nuniform sampler2D sTex;\nuniform sampler2D sTex1;\nvoid main(){}";
  // Replacement that correctly declares both:
  const std::string_view good = "uniform sampler2D sTex;\nuniform sampler2D sTex1;\nvoid main(){}";
  // Replacement that omits sTex (only has sTex1 — which contains "sTex" as substring):
  const std::string_view bad_stex_only1 = "uniform sampler2D sTex1;\nvoid main(){}";
  expect_true(iee::game::check_interface_contract(original, good).ok,
              "both sTex and sTex1 present -> passes");
  const auto failed = iee::game::check_interface_contract(original, bad_stex_only1);
  expect_true(!failed.ok, "sTex1 must not satisfy sTex contract (token-boundary check)");
  expect_eq(failed.missingIdentifiers.size(), std::size_t{1}, "only sTex missing");
  expect_eq(failed.missingIdentifiers[0], std::string("sTex"), "missing identifier is sTex");
}
}  // namespace

void test_area_liquid_texture_packing() {
  iee::game::WedAreaInfo wed{};
  wed.baseWidth = 2;
  wed.baseHeight = 2;
  wed.overlays.resize(3);
  wed.overlays[1].liquidMode = iee::game::TileLiquidMode::Water;
  wed.overlays[2].liquidMode = iee::game::TileLiquidMode::Lava;
  // cell flags: bit N set = overlay N covers the cell
  wed.baseOverlayFlags = {
      0x00,  // no overlays -> mode 0
      0x02,  // overlay 1 (water) -> mode 1
      0x04,  // overlay 2 (lava) -> mode 2
      0x06,  // overlays 1+2 -> lowest overlay index wins -> mode 1
  };
  const auto packed = iee::game::pack_area_liquid_texture(wed);
  expect_true(packed.has_value(), "packs valid wed");
  if (packed) {
    expect_eq(packed->width, 2, "liquid width");
    expect_eq(packed->height, 2, "liquid height");
    expect_eq(packed->texels[0], std::uint8_t{0}, "no overlay -> 0");
    expect_eq(packed->texels[1], std::uint8_t{1}, "water overlay -> 1");
    expect_eq(packed->texels[2], std::uint8_t{2}, "lava overlay -> 2");
    expect_eq(packed->texels[3], std::uint8_t{1}, "first liquid overlay wins");
  }
}

void test_area_liquid_texture_packing_rejects_mismatch() {
  iee::game::WedAreaInfo wed{};
  wed.baseWidth = 3;
  wed.baseHeight = 1;
  // assign() instead of a 1-element initializer list: GCC 13 -O2 emits a
  // false-positive -Warray-bounds on the list's backing-array copy.
  wed.baseOverlayFlags.assign(1, 0x00);  // wrong size
  expect_true(!iee::game::pack_area_liquid_texture(wed).has_value(),
              "flag/dimension mismatch -> nullopt");
  iee::game::WedAreaInfo empty{};
  expect_true(!iee::game::pack_area_liquid_texture(empty).has_value(), "empty -> nullopt");
}

void test_fpseam_override_asset_contract() {
  namespace fs = std::filesystem;
  const fs::path assetPath = fs::path("assets") / "override" / "fpSEAM.glsl";
  std::ifstream file(assetPath, std::ios::binary);
  expect_true(static_cast<bool>(file), "fpSEAM override asset exists (run tests from repo root)");
  if (!file) return;
  std::ostringstream contents;
  contents << file.rdbuf();
  const std::string source = contents.str();

  // Engine interface (from the live vanilla dump) must be fully preserved.
  constexpr std::string_view vanillaInterface =
      "uniform sampler2D uTex;\n"
      "uniform vec2 uTcScale;\n"
      "uniform vec4 uColorTone;\n"
      "varying vec2 vTc;\n"
      "varying vec2 vRef;\n"
      "varying vec4 vColor;\n";
  const auto contract = iee::game::check_interface_contract(vanillaInterface, source);
  expect_true(contract.ok, "fpSEAM override preserves the engine interface");
  for (const auto& missing : contract.missingIdentifiers) {
    std::cerr << "  missing identifier: " << missing << '\n';
  }

  // Our feed contract.
  for (const std::string_view name :
       {"uIeeEnabled", "uIeeTime", "uIeeScroll", "uIeeZoom", "uIeeViewport", "uIeeWorldSizeInv",
        "uIeeWaterTint", "uIeePointCount", "uIeePoints", "uIeeAreaMask", "uIeeNormalMap",
        "uIeeDudvMap", "uIeeFoamMap", "uIeeNoiseMap"}) {
    expect_true(source.find(name) != std::string::npos, "fpSEAM override declares feed uniform");
  }
  // The uniform-array capacity in the shader must match the bridge/packing
  // cap (two vec4 slots per point).
  expect_true(source.find("uIeePoints[64]") != std::string::npos,
              "fpSEAM point array capacity matches kMaxAreaEffectPoints * 2");
  expect_true(source.find("#version") == std::string::npos,
              "no #version line (engine sources are ARB-era GLSL)");
  expect_true(
      source.find("ieeCoverageWithCenter") != std::string::npos &&
          source.find("ieeShoreFactor(vec2 worldPos, float centerCoverage)") != std::string::npos,
      "water shader should reuse center coverage instead of resampling it");
  expect_true(source.find("ieeSrgbToLinear") != std::string::npos &&
                  source.find("ieeLinearToSrgb") != std::string::npos,
              "water grading should explicitly cross the encoded/linear color boundary");
  expect_true(source.find("min(edgeDistance.x, edgeDistance.y) > 8.0") != std::string::npos,
              "interior land fragments should skip provably redundant coverage fetches");
  expect_true(source.find("if (ieeIsInteriorWater(worldPos, centerCoverage))") !=
                      std::string::npos &&
                  source.find("vec2(-cell, -cell)") != std::string::npos,
              "confirmed interior water should skip the shoreline filter");
}

void test_classify_area_animation() {
  using iee::game::AreaAnimationKind;
  using iee::game::classify_area_animation;

  expect_true(classify_area_animation("FLAMBIG", "") == AreaAnimationKind::Fire,
              "FLAM* resrefs should classify as fire");
  expect_true(classify_area_animation("torch01", "") == AreaAnimationKind::Fire,
              "Resref classification should be case-insensitive");
  expect_true(classify_area_animation("ZZANIM", "Village fireplace") == AreaAnimationKind::Fire,
              "Authored names should classify when the resref does not");
  expect_true(classify_area_animation("FPIT1S", "FPIT1S") == AreaAnimationKind::Fire,
              "Fire pits (BG2EE AR0406) should classify as fire");
  expect_true(classify_area_animation("FLMSW", "FLMSW") == AreaAnimationKind::Fire,
              "Bare FLM* flame BAMs (BG1EE) should classify as fire");
  expect_true(classify_area_animation("FLMS", "Candle03") == AreaAnimationKind::Light,
              "Candle-named flames (BG1EE FLMS family) are dim lights, not fires");
  expect_true(classify_area_animation("FLMM", "Sconce01") == AreaAnimationKind::Fire,
              "Sconces are wall flames");
  expect_true(classify_area_animation("AR900WN1", "AR900WN1") == AreaAnimationKind::None,
              "AR<area>W[DN]* overlays are night/day shadow scenery (verified from frames)");
  expect_true(classify_area_animation("AR900WD1", "AR900WD1") == AreaAnimationKind::None,
              "Day shadow overlays stay unclassified scenery");
  expect_true(classify_area_animation("FIM1YLN1", "FIM1YLN1") == AreaAnimationKind::Fire,
              "FIM* yellow flames (verified from frames) classify as fire");
  expect_true(classify_area_animation("YSFLBLU2", "YSFLBLU2") == AreaAnimationKind::Fire,
              "YSFL* blue flames (verified from frames) classify as fire");
  expect_true(classify_area_animation("AM003XA", "AM003XA") == AreaAnimationKind::Fire,
              "The hearth overlay is an exact-resref fire");
  expect_true(classify_area_animation("AM5508C", "AM5508C") == AreaAnimationKind::Light,
              "The glow orb overlay is an exact-resref light");
  expect_true(classify_area_animation("AM6004A", "AM6004A") == AreaAnimationKind::Smoke,
              "The dark plume overlay is an exact-resref smoke");
  expect_true(classify_area_animation("AM0604A", "AM0604A") == AreaAnimationKind::Fountain,
              "The tiered fountain overlay is an exact-resref fountain");
  expect_true(classify_area_animation("AM0202FL", "AM0202FL") == AreaAnimationKind::Light,
              "The star glint overlay is a light, not a flame, despite the FL suffix");
  expect_true(classify_area_animation("SPLASH", "SPLASH") == AreaAnimationKind::Water,
              "Splashes classify as water effects");
  expect_true(classify_area_animation("DS6000W3", "Waterfall") == AreaAnimationKind::Water,
              "Waterfall names classify as water effects");
  expect_true(classify_area_animation("BD0130LL", "Lava_Left") == AreaAnimationKind::Lava,
              "Lava names classify as lava");
  expect_true(classify_area_animation("FISH3S", "Fish") == AreaAnimationKind::Wildlife,
              "Fish classify as wildlife");
  expect_true(classify_area_animation("FLIESS", "FLIESS") == AreaAnimationKind::Wildlife,
              "Fly swarms classify as wildlife");
  expect_true(classify_area_animation("BUTRFLY", "BUTRFLY3") == AreaAnimationKind::Wildlife,
              "Butterflies classify as wildlife");
  expect_true(classify_area_animation("BD5100M1", "Mist_BD5100M1") == AreaAnimationKind::Smoke,
              "Authored mist folds into the smoke kind");
  expect_true(classify_area_animation("AMSTEAM1", "AMB_Pipe1A") == AreaAnimationKind::Smoke,
              "Steam pipes (BG2EE AR3017) fold into the smoke kind");
  expect_true(classify_area_animation("BUBBLES2", "BUBBLES2") == AreaAnimationKind::Water,
              "Bubbles (BG2EE sewers) classify as water effects");
  expect_true(classify_area_animation("AMOH7300", "Tank_Bubbles") == AreaAnimationKind::Water,
              "Bubble-named overlays classify as water effects");
  expect_true(classify_area_animation("SMOKE2", "") == AreaAnimationKind::Smoke,
              "SMOK* resrefs should classify as smoke");
  expect_true(classify_area_animation("ZZANIM", "chimney smoke") == AreaAnimationKind::Smoke,
              "Chimney names should classify as smoke");
  expect_true(classify_area_animation("FOUNT1", "") == AreaAnimationKind::Fountain,
              "FOUNT* resrefs should classify as fountain");
  expect_true(classify_area_animation("GLOW01", "") == AreaAnimationKind::Light,
              "GLOW* resrefs should classify as light");
  expect_true(classify_area_animation("ZZANIM", "window light") == AreaAnimationKind::Light,
              "Light names should classify as light");
  expect_true(classify_area_animation("ZZANIM", "lightning strike") == AreaAnimationKind::None,
              "Lightning is weather, not an authored light source");
  expect_true(classify_area_animation("ZZANIM", "mystery") == AreaAnimationKind::None,
              "Unknown entries must stay unclassified");
  expect_true(classify_area_animation("", "") == AreaAnimationKind::None,
              "Empty input classifies as none");
}

void test_parse_are_animations() {
  using namespace iee::game;

  ARE_Header_st header{};
  header.nFileType = 0x41455241;     // "AREA"
  header.nFileVersion = 0x302E3156;  // "V1.0"
  header.nAnimations = 2;
  header.nAnimationsOffset = sizeof(ARE_Header_st);

  ARE_Animation_st fire{};
  const char fireName[] = "Fireplace big";
  std::memcpy(fire.szName.data(), fireName, sizeof(fireName) - 1);
  fire.nX = 320;
  fire.nY = 240;
  fire.nHeight = 5;
  fire.rrAnimation = {'F', 'L', 'A', 'M', 'B', 'I', 'G', 0};
  fire.nFlags = kAreAnimationFlagIsShown;
  fire.nSchedule = 0x00FFFFFF;

  ARE_Animation_st unknown{};
  const char unknownName[] = "mystery";
  std::memcpy(unknown.szName.data(), unknownName, sizeof(unknownName) - 1);
  unknown.rrAnimation = {'Z', 'Z', 'X', 'Y', 0, 0, 0, 0};
  unknown.nFlags = kAreAnimationFlagNotLightSource;

  std::vector<std::byte> bytes;
  write_bytes(bytes, 0, &header, sizeof(header));
  write_bytes(bytes, sizeof(ARE_Header_st), &fire, sizeof(fire));
  write_bytes(bytes, sizeof(ARE_Header_st) + sizeof(ARE_Animation_st), &unknown, sizeof(unknown));

  AreaAnimationsInfo info{};
  expect_true(parse_are_animations(bytes.data(), bytes.size(), info),
              "A valid ARE V1.0 animation section should parse");
  expect_eq(info.animations.size(), std::size_t{2}, "Both animation records should be read");
  expect_true(info.animations[0].kind == AreaAnimationKind::Fire,
              "The FLAM* record should classify as fire");
  expect_true(info.animations[0].resrefView() == "FLAMBIG", "Animation resref should round-trip");
  expect_true(info.animations[0].nameView() == "Fireplace big",
              "Animation name should round-trip NUL-terminated");
  expect_eq(info.animations[0].x, std::uint16_t{320}, "Animation X coordinate should round-trip");
  expect_eq(info.animations[0].y, std::uint16_t{240}, "Animation Y coordinate should round-trip");
  expect_true(info.animations[0].isShown(), "Flag bit 0 should report as shown");
  expect_true(info.animations[0].isLightSource(),
              "An animation without the not-light-source bit is a light source");
  expect_true(!info.animations[1].isShown(), "Missing flag bit 0 should report as not shown");
  expect_true(!info.animations[1].isLightSource(),
              "The not-light-source bit should suppress light-source status");
  expect_eq(info.count_of(AreaAnimationKind::Fire), std::size_t{1},
            "count_of should tally classified kinds");
  expect_eq(info.count_of(AreaAnimationKind::None), std::size_t{1},
            "count_of should tally unclassified records");

  // Zero animations is a valid area.
  auto emptyHeader = header;
  emptyHeader.nAnimations = 0;
  emptyHeader.nAnimationsOffset = 0;
  std::vector<std::byte> emptyBytes;
  write_bytes(emptyBytes, 0, &emptyHeader, sizeof(emptyHeader));
  AreaAnimationsInfo emptyInfo{};
  expect_true(parse_are_animations(emptyBytes.data(), emptyBytes.size(), emptyInfo),
              "An ARE without animations should parse");
  expect_true(emptyInfo.animations.empty(), "An ARE without animations should yield no records");

  // Unsupported version (IWD2 V9.1 shifts the section offsets).
  auto v91 = header;
  v91.nFileVersion = 0x312E3956;  // "V9.1"
  std::vector<std::byte> v91Bytes(bytes);
  write_bytes(v91Bytes, 0, &v91, sizeof(v91));
  AreaAnimationsInfo v91Info{};
  expect_true(!parse_are_animations(v91Bytes.data(), v91Bytes.size(), v91Info),
              "Non-V1.0 ARE versions must fail closed");

  // Truncated section: count says two records but only one fits.
  std::vector<std::byte> truncated(bytes.begin(),
                                   bytes.end() - static_cast<std::ptrdiff_t>(sizeof(fire)));
  AreaAnimationsInfo truncatedInfo{};
  expect_true(!parse_are_animations(truncated.data(), truncated.size(), truncatedInfo),
              "A truncated animation section must fail closed");
  expect_true(truncatedInfo.animations.empty(), "A failed parse must leave the output empty");

  // Malicious count.
  auto hugeHeader = header;
  hugeHeader.nAnimations = 1'000'000;
  std::vector<std::byte> hugeBytes(bytes);
  write_bytes(hugeBytes, 0, &hugeHeader, sizeof(hugeHeader));
  AreaAnimationsInfo hugeInfo{};
  expect_true(!parse_are_animations(hugeBytes.data(), hugeBytes.size(), hugeInfo),
              "An implausible animation count must fail closed");

  AreaAnimationsInfo shortInfo{};
  expect_true(!parse_are_animations(bytes.data(), sizeof(ARE_Header_st) - 1, shortInfo),
              "A buffer smaller than the ARE header must fail closed");
}

void test_decode_object_array_globals() {
  using namespace iee::game;

  // Synthetic CGameObjectArray::GetShare body: manifest pattern prologue,
  // then the RIP-relative max-index compare, the (ignored) next-id compare,
  // and the entry-table lea, each pointing at slots inside the same buffer.
  std::array<std::byte, 0x100> code{};
  const auto put = [&](std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
    std::size_t index = offset;
    for (const auto value : bytes) code[index++] = static_cast<std::byte>(value);
  };
  const auto putRip = [&](std::size_t offset, std::initializer_list<std::uint8_t> opcode,
                          std::size_t target) {
    put(offset, opcode);
    const auto displacement = static_cast<std::int32_t>(static_cast<std::ptrdiff_t>(target) -
                                                        static_cast<std::ptrdiff_t>(offset + 7));
    std::memcpy(code.data() + offset + 3, &displacement, sizeof(displacement));
  };
  put(0, {0x48, 0xC7, 0x02, 0x00, 0x00, 0x00, 0x00, 0x83, 0xF9, 0xFF});
  putRip(10, {0x66, 0x39, 0x05}, 0x80);  // cmp [rip+d], ax -> m_maxArrayIndex
  putRip(17, {0x66, 0x39, 0x0D}, 0x84);  // cmp [rip+d], cx -> ignored
  putRip(24, {0x4C, 0x8D, 0x05}, 0x90);  // lea r8, [rip+d] -> entry table

  ObjectArrayGlobals globals{};
  expect_true(decode_object_array_globals(code.data(), 0x60, globals),
              "GetShare RIP operands should decode from a well-formed body");
  expect_true(reinterpret_cast<const std::byte*>(globals.maxArrayIndex) == code.data() + 0x80,
              "The max-index compare operand should decode to its RIP target");
  expect_true(reinterpret_cast<const std::byte*>(globals.entries) == code.data() + 0x90,
              "The entry-table lea operand should decode to its RIP target");

  ObjectArrayGlobals tooSmall{};
  expect_true(!decode_object_array_globals(code.data(), 0x10, tooSmall),
              "A window without both instructions must fail closed");

  putRip(40, {0x66, 0x39, 0x05}, 0x88);  // duplicate max-index compare
  ObjectArrayGlobals ambiguous{};
  expect_true(!decode_object_array_globals(code.data(), 0x60, ambiguous),
              "Ambiguous instruction matches must fail closed");
}

void test_collect_area_static_animations() {
  using namespace iee::game;

  CGameArea areaA{};
  CGameArea areaB{};
  std::array<CGameStatic, 3> statics{};

  frameTableEntry_st liveFrame{};
  liveFrame.nWidth = 12;
  liveFrame.nHeight = 40;
  liveFrame.nCenterX = 6;
  liveFrame.nCenterY = 30;

  statics[0].baseclass_0.m_objectType = kGameObjectTypeStatic;
  statics[0].baseclass_0.m_pArea = &areaA;
  statics[0].m_vidCell.m_pFrame = &liveFrame;
  statics[0].m_header.rrAnimation = {'F', 'L', 'A', 'M', 'B', 'I', 'G', 0};
  const char fireName[] = "FLAMBIG";
  std::memcpy(statics[0].m_header.szName.data(), fireName, sizeof(fireName) - 1);
  statics[0].m_header.nX = 320;
  statics[0].m_header.nY = 240;
  statics[0].m_header.nFlags = kAreAnimationFlagIsShown;

  // Same type, different area: filtered.
  statics[1].baseclass_0.m_objectType = kGameObjectTypeStatic;
  statics[1].baseclass_0.m_pArea = &areaB;
  statics[1].m_header.rrAnimation = {'S', 'M', 'O', 'K', 'E', '2', 0, 0};

  // Same area, different object type: filtered.
  statics[2].baseclass_0.m_objectType = 0x31;
  statics[2].baseclass_0.m_pArea = &areaA;

  std::array<CGameObjectArrayEntry, 6> entries{};
  entries[1].m_objectPtr = &statics[0].baseclass_0;
  entries[3].m_objectPtr = &statics[1].baseclass_0;
  entries[4].m_objectPtr = &statics[2].baseclass_0;

  std::int16_t maxIndex = 5;
  const ObjectArrayGlobals globals{entries.data(), &maxIndex};

  AreaAnimationsInfo out{};
  expect_true(collect_area_static_animations(globals, &areaA, out),
              "A readable object array should collect");
  expect_eq(out.animations.size(), std::size_t{1},
            "Only statics owned by the requested area should be collected");
  expect_true(!out.animations.empty() && out.animations[0].kind == AreaAnimationKind::Fire,
              "Collected records should classify like the disk parser");
  expect_true(!out.animations.empty() && out.animations[0].x == 320 &&
                  out.animations[0].isShown(),
              "Collected records should carry the live header fields");
  expect_true(!out.animations.empty() && out.animations[0].frameValid &&
                  out.animations[0].frameWidth == 12 && out.animations[0].frameHeight == 40 &&
                  out.animations[0].frameCenterX == 6 && out.animations[0].frameCenterY == 30,
              "The walk mirrors the engine's cached CVidCell frame geometry");

  AreaAnimationsInfo invalidOut{};
  expect_true(!collect_area_static_animations(ObjectArrayGlobals{}, &areaA, invalidOut),
              "Unresolved globals must fail closed");
  std::int16_t negativeIndex = -1;
  const ObjectArrayGlobals negative{entries.data(), &negativeIndex};
  expect_true(!collect_area_static_animations(negative, &areaA, invalidOut),
              "A negative max index must fail closed");
}

void test_build_area_effect_points() {
  using namespace iee::game;

  AreaAnimationsInfo info{};
  const auto add = [&](AreaAnimationKind kind, const char* resref, std::uint16_t x, bool shown) {
    AreaAnimationInfo animation{};
    animation.kind = kind;
    animation.x = x;
    animation.y = 100;
    animation.objX = x;
    animation.objY = 100;
    animation.flags = shown ? kAreAnimationFlagIsShown : 0;
    for (std::size_t c = 0; resref[c] != '\0' && c < 8; ++c) animation.resref[c] = resref[c];
    info.animations.push_back(animation);
  };

  add(AreaAnimationKind::Smoke, "CHIMSMK", 10, true);
  add(AreaAnimationKind::Smoke, "AM6004A", 15, true);   // authored plume art: no point
  add(AreaAnimationKind::Fire, "FIRE_4", 20, true);
  add(AreaAnimationKind::Fire, "flamblu2", 25, true);   // live lowercase resref: blue + shift
  add(AreaAnimationKind::Fire, "AM5204C", 28, true);    // hearth overlay: glow only
  add(AreaAnimationKind::Fire, "FIRE_4", 30, false);    // hidden: excluded
  add(AreaAnimationKind::Light, "FLMS", 40, true);
  add(AreaAnimationKind::Wildlife, "FISH3S", 50, true);  // no effect kind: excluded
  add(AreaAnimationKind::Water, "SPLASH", 60, true);     // water path: excluded

  // Engine-native geometry wins when the walk read the live frame entry.
  {
    AreaAnimationsInfo live{};
    AreaAnimationInfo animation{};
    animation.kind = AreaAnimationKind::Fire;
    animation.objX = 100;
    animation.objY = 200;
    animation.flags = kAreAnimationFlagIsShown;
    const char liveResref[] = "flamblu2";
    for (std::size_t c = 0; liveResref[c] != '\0'; ++c) animation.resref[c] = liveResref[c];
    animation.frameValid = true;
    animation.frameWidth = 8;
    animation.frameHeight = 15;
    animation.frameCenterX = 0;
    animation.frameCenterY = 0;
    live.animations.push_back(animation);
    const auto livePoints = build_area_effect_points(live);
    expect_true(livePoints.size() == 1 && livePoints[0].x == 100.0f &&
                    livePoints[0].y == 200.0f && livePoints[0].height == 15.0f &&
                    livePoints[0].halfWidth == 4.0f && livePoints[0].reserved1 == 1.0f,
                "Live CVidCell frame geometry sizes the flame; the anchor is unshifted");
  }

  const auto points = build_area_effect_points(info);
  expect_eq(points.size(), std::size_t{5}, "Shown fire/light + replaceable smoke become points");
  expect_true(!points.empty() && points[0].kind == 1.0f && points[0].x == 20.0f &&
                  points[0].height == 27.0f && points[0].halfWidth == 7.0f,
              "Fire points come first with authored BAM geometry");
  expect_true(points.size() >= 2 && points[1].kind == 1.0f && points[1].reserved1 == 1.0f &&
                  points[1].x == 25.0f && points[1].y == 100.0f &&
                  points[1].height == 15.0f && points[1].halfWidth == 4.0f,
              "Blue flames carry the palette id and the authored footprint at the unshifted anchor");
  expect_true(points.size() >= 3 && points[2].kind == 1.0f && points[2].reserved1 == 2.0f,
              "Overlay fires become glow-only points");
  expect_true(points.size() >= 4 && points[3].kind == 4.0f,
              "Light points follow fire");
  expect_true(points.size() >= 5 && points[4].kind == 2.0f && points[4].x == 10.0f,
              "Only standalone smoke BAMs become plume points");

  AreaAnimationsInfo overflow{};
  for (int i = 0; i < 80; ++i) {
    AreaAnimationInfo animation{};
    animation.kind = i < 40 ? AreaAnimationKind::Smoke : AreaAnimationKind::Fire;
    animation.resref[0] = 'F';
    animation.resref[1] = 'L';
    animation.resref[2] = 'A';
    animation.resref[3] = 'M';
    if (i < 40) {
      animation.resref[0] = 'S';
      animation.resref[1] = 'M';
      animation.resref[2] = 'O';
      animation.resref[3] = 'K';
    }
    animation.flags = kAreAnimationFlagIsShown;
    overflow.animations.push_back(animation);
  }
  const auto capped = build_area_effect_points(overflow);
  expect_eq(capped.size(), kMaxAreaEffectPoints, "The point set is capped at the uniform size");
  expect_true(!capped.empty() && capped[0].kind == 1.0f,
              "Under capacity pressure, fire wins over smoke");
}

void test_config_detection_section() {
  const auto tempPath =
      std::filesystem::current_path() / "InfinityEngine-Enhancer-detection-test.ini";
  {
    std::ofstream out(tempPath, std::ios::trunc);
    out << "[Detection]\n";
    out << "AreaAnimationScan = false\n";
  }

  iee::core::EngineConfig cfg{};
  expect_true(cfg.enableAreaAnimationScan, "The area animation scan should default to enabled");
  expect_true(iee::core::ConfigManager::load(tempPath, cfg),
              "ConfigManager::load should parse the detection section");
  expect_true(!cfg.enableAreaAnimationScan, "AreaAnimationScan=false should disable the scan");

  expect_true(iee::core::ConfigManager::save(tempPath, cfg),
              "ConfigManager::save should persist the detection section");
  iee::core::EngineConfig reloaded{};
  expect_true(iee::core::ConfigManager::load(tempPath, reloaded),
              "The saved detection section should reload");
  expect_true(!reloaded.enableAreaAnimationScan, "AreaAnimationScan should round-trip");

  std::error_code error;
  std::filesystem::remove(tempPath, error);
}

int main() {
  test_parse_ida_pattern();
  test_unique_pattern_matching();
  test_detour_tolerant_matching();
  test_rel32_target_checked();
  test_manifest_loading();
  test_runtime_type_layouts();
  test_file_format_layouts();
  test_eeex_doc_layout_maps();
  test_config_parsing();
  test_config_numeric_bounds();
  test_config_reports_malformed_values();
  test_config_shader_override_defaults();
  test_config_shader_override_roundtrip();
  test_performance_sample_summary();
  test_parse_dds_legacy_formats_and_mips();
  test_parse_dds_dx10_formats();
  test_parse_dds_rejects_unsupported_or_malformed_input();
  test_load_dds_texture_file_wrapper();
  test_parse_loaded_wed();
  test_wed_tint_candidates_are_bounded();
  test_decode_palette_tile_alpha();
  test_tis_header_dimension_decoding();
  test_supported_tile_dimensions_are_inferred_dynamically();
  test_tis_table_entry_bounds();
  test_tileset_runtime_cache_is_bounded_and_resettable();
  test_scale_selection_precedence();
  test_tile_table_detection_ignores_garbage_steps();
  test_tile_table_detection_uses_coordinate_deltas();
  test_manifest_infgame_offsets();
  test_shader_name_extraction();
  test_interface_contract();
  test_interface_contract_token_boundary();
  test_area_liquid_texture_packing();
  test_area_liquid_texture_packing_rejects_mismatch();
  test_fpseam_override_asset_contract();
  test_classify_area_animation();
  test_parse_are_animations();
  test_decode_object_array_globals();
  test_collect_area_static_animations();
  test_build_area_effect_points();
  test_config_detection_section();

  if (g_failures != 0) {
    std::cerr << g_failures << " test(s) failed\n";
    return 1;
  }

  std::cout << "All InfinityEngine-Enhancer native tests passed\n";
  return 0;
}
