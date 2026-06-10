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
        const char *name{};
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

    struct FallbackRvas {
        std::uintptr_t loadArea{};
        std::uintptr_t renderTexture{};
    };

    struct RuntimeOffsets {
        std::uintptr_t vidTileResource{};
        std::uintptr_t tisLinearTilesFlag{};
        std::uintptr_t tisHeaderTileDimension{};
    };

    struct BuildManifest {
        std::string_view buildId{};
        std::array<std::string_view, 2> supportedGames{};
        PatternSet patterns{};
        FallbackRvas fallbacks{};
        RuntimeOffsets offsets{};
        std::array<BranchInstructionDesc, 11> renderTextureCallsites{};

        [[nodiscard]] constexpr bool validate() const noexcept {
            if (buildId.empty() || patterns.loadArea.empty() || patterns.renderTexture.empty()) {
                return false;
            }
            if (!fallbacks.loadArea || !fallbacks.renderTexture) {
                return false;
            }
            if (!offsets.vidTileResource || !offsets.tisLinearTilesFlag || !offsets.tisHeaderTileDimension) {
                return false;
            }

            for (const auto &callsite: renderTextureCallsites) {
                if (!callsite.validate()) {
                    return false;
                }
            }

            return true;
        }
    };

    [[nodiscard]] const BuildManifest &current_manifest() noexcept;

    [[nodiscard]] std::optional<std::reference_wrapper<const BuildManifest> > find_manifest(std::string_view buildId)
    noexcept;

    [[nodiscard]] const BuildManifest *detect_manifest() noexcept;
}
