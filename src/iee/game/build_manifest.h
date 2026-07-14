#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace iee::game {
enum class BranchInstructionKind : std::uint8_t {
  CallRel32,
  JmpRel32,
};

struct BranchInstructionDesc {
  const char* name{};
  std::size_t offset{};
  BranchInstructionKind kind{BranchInstructionKind::CallRel32};
  std::uint8_t opcode{};
  std::size_t displacementOffset{};
  std::size_t instructionSize{};
  bool required{true};

  [[nodiscard]] constexpr bool validate() const noexcept {
    return name != nullptr && name[0] != '\0' && instructionSize > displacementOffset;
  }
};

struct PatternSet {
  std::string_view loadArea{};
  std::string_view renderTexture{};
};

struct ReferenceRvas {
  std::uintptr_t loadArea{};
  std::uintptr_t renderTexture{};
};

struct RuntimeOffsets {
  std::uintptr_t vidTileResource{};
  std::uintptr_t tisLinearTilesFlag{};
  std::uintptr_t tisHeaderTileDimension{};
  std::uintptr_t infGameVisibleArea{};
  std::uintptr_t infGameAreas{};
  std::uintptr_t infGameAreaMaster{};
};

struct ExecutableVersion {
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint16_t patch{};

  [[nodiscard]] constexpr bool matches(std::uint16_t candidateMajor, std::uint16_t candidateMinor,
                                       std::uint16_t candidatePatch) const noexcept {
    return major == candidateMajor && minor == candidateMinor && patch == candidatePatch;
  }
};

struct BuildManifest {
  std::string_view buildId{};
  std::array<std::string_view, 2> supportedGames{};
  ExecutableVersion executableVersion{};
  PatternSet patterns{};
  ReferenceRvas referenceRvas{};
  RuntimeOffsets offsets{};
  std::array<BranchInstructionDesc, 11> renderTextureCallsites{};

  [[nodiscard]] constexpr bool validate() const noexcept {
    if (buildId.empty() || executableVersion.major == 0 || patterns.loadArea.empty() ||
        patterns.renderTexture.empty()) {
      return false;
    }
    if (!referenceRvas.loadArea || !referenceRvas.renderTexture) {
      return false;
    }
    if (!offsets.vidTileResource || !offsets.tisLinearTilesFlag ||
        !offsets.tisHeaderTileDimension) {
      return false;
    }
    if (!offsets.infGameVisibleArea || !offsets.infGameAreas || !offsets.infGameAreaMaster) {
      return false;
    }

    for (const auto& callsite : renderTextureCallsites) {
      if (!callsite.validate()) {
        return false;
      }
    }

    return true;
  }
};

[[nodiscard]] const BuildManifest& current_manifest() noexcept;

[[nodiscard]] std::optional<std::reference_wrapper<const BuildManifest>> find_manifest(
    std::string_view buildId) noexcept;

[[nodiscard]] std::optional<std::reference_wrapper<const BuildManifest>> find_manifest_for_version(
    std::uint16_t major, std::uint16_t minor, std::uint16_t patch) noexcept;

// Selects a manifest from the main executable's fixed file version. Unknown
// versions are deliberately unsupported and return nullptr before scanning
// or installing any hooks.
[[nodiscard]] const BuildManifest* detect_manifest(
    ExecutableVersion* detectedVersion = nullptr) noexcept;
}  // namespace iee::game
