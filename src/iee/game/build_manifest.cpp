#include "build_manifest.h"

namespace iee::game {
    namespace {
        constexpr bool is_hex_char(char c) noexcept {
            return (c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'F') ||
                   (c >= 'a' && c <= 'f');
        }

        constexpr bool is_wildcard_token(std::string_view token) noexcept {
            return token == "?" || token == "??";
        }

        constexpr bool is_hex_token(std::string_view token) noexcept {
            return token.size() == 2 && is_hex_char(token[0]) && is_hex_char(token[1]);
        }

        constexpr bool validate_pattern_format(std::string_view pattern) noexcept {
            if (pattern.empty()) {
                return false;
            }

            std::size_t tokenStart = 0;
            bool sawToken = false;
            while (tokenStart < pattern.size()) {
                while (tokenStart < pattern.size() && pattern[tokenStart] == ' ') {
                    ++tokenStart;
                }
                if (tokenStart >= pattern.size()) {
                    break;
                }

                std::size_t tokenEnd = tokenStart;
                while (tokenEnd < pattern.size() && pattern[tokenEnd] != ' ') {
                    ++tokenEnd;
                }

                const auto token = pattern.substr(tokenStart, tokenEnd - tokenStart);
                if (!is_hex_token(token) && !is_wildcard_token(token)) {
                    return false;
                }

                sawToken = true;
                tokenStart = tokenEnd;
            }

            return sawToken;
        }
        constexpr BuildManifest kKnownBuilds[] = {
            {
                "BGEE 2.6.6.x",
                {"BGEE", ""},
                {
                    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 FD FF FF",
                    "48 8B C4 44 89 48 20 48 83 EC 48 48 89 58 08 8B DA 48 89 68 10",
                },
                {0x27E710, 0x4247E0},
                {0x100, 0x1DC, 0x14},
                {{
                    {"CRes_Demand", 0x36, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawBindTexture", 0x6E, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawDisable", 0x7F, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawColor", 0x89, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawPushState", 0x91, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawColorTone", 0xB6, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawBegin", 0xC0, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawTexCoord", 0xCD, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawVertex", 0xDB, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawEnd", 0x17A, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
                    {"DrawPopState", 0x1AD, BranchInstructionKind::JmpRel32, 0xE9, 1, 5, true},
                }},
            },
        };

        static_assert(validate_pattern_format(kKnownBuilds[0].patterns.loadArea),
                      "LoadArea pattern format is invalid");
        static_assert(validate_pattern_format(kKnownBuilds[0].patterns.renderTexture),
                      "RenderTexture pattern format is invalid");
        static_assert(kKnownBuilds[0].validate(), "Known build manifest is invalid");
    }

    const BuildManifest &current_manifest() noexcept {
        return kKnownBuilds[0];
    }

    std::optional<std::reference_wrapper<const BuildManifest> > find_manifest(std::string_view buildId) noexcept {
        for (const auto &manifest: kKnownBuilds) {
            if (manifest.buildId == buildId) {
                return manifest;
            }
        }
        return std::nullopt;
    }

    const BuildManifest *detect_manifest() noexcept {
        return &current_manifest();
    }
}
