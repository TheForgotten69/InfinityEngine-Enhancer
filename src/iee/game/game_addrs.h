#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace iee {
    namespace core { struct EngineConfig; }
}

namespace iee::game {

    // region Pattern Signatures
    // IDA-style patterns for key game functions
    namespace PatternSignatures {
        // LoadArea function - handles area loading and initialization
        constexpr const char* LOAD_AREA = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 FD FF FF";

        // RenderTexture function - core tile rendering function
        constexpr const char* RENDER_TEXTURE = "48 8B C4 44 89 48 20 48 83 EC 48 48 89 58 08 8B DA 48 89 68 10";

        // Compile-time pattern validation helpers
        namespace detail {
            constexpr bool is_hex_char(char c) {
                return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
            }

            constexpr bool validate_pattern_format(const char* pattern) {
                if (!pattern || *pattern == '\0') return false;

                int hex_count = 0;
                for (const char* p = pattern; *p; ++p) {
                    if (*p == ' ') {
                        // Space should come after exactly 2 hex chars (complete byte)
                        if (hex_count != 2) return false;
                        hex_count = 0;
                    } else if (is_hex_char(*p)) {
                        hex_count++;
                        if (hex_count > 2) return false; // More than 2 hex chars without space
                    } else {
                        return false; // Invalid character
                    }
                }
                // Pattern should end with exactly 2 hex chars (complete byte)
                return hex_count == 2;
            }
        }

        // Compile-time validation - will cause compilation error if patterns are malformed
        static_assert(detail::validate_pattern_format(LOAD_AREA), "LOAD_AREA pattern format is invalid");
        static_assert(detail::validate_pattern_format(RENDER_TEXTURE), "RENDER_TEXTURE pattern format is invalid");
    }
    // endregion

    struct GameAddresses {
        std::uintptr_t LoadArea = 0;
        std::uintptr_t RenderTexture = 0;
        bool initialized = false;
    };

    // Game object structure offsets (centralized magic numbers)
    struct GameOffsets {
        // Video tile object offsets
        static constexpr uintptr_t VID_TILE_RESOURCE_OFFSET = 0x100;

        // TIS structure offsets
        static constexpr uintptr_t TIS_LINEAR_TILES_OFFSET = 0x1DC;

        // RenderTexture function offsets for draw API resolution
        // NOTE: All RT_* offsets assume E8 rel32 call instructions (disp_offset=1, size=5)
        // If any offset points to a different instruction type (LEA, MOV, etc.),
        // update resolve_draw_api() to handle those cases with correct disp_offset/size
        static constexpr uintptr_t RT_CRES_DEMAND_OFFSET = 0x36;
        static constexpr uintptr_t RT_DRAW_BIND_TEXTURE_OFFSET = 0x6E;
        static constexpr uintptr_t RT_DRAW_DISABLE_OFFSET = 0x7F;
        static constexpr uintptr_t RT_DRAW_COLOR_OFFSET = 0x89;
        static constexpr uintptr_t RT_DRAW_PUSH_STATE_OFFSET = 0x91;
        static constexpr uintptr_t RT_DRAW_COLOR_TONE_OFFSET = 0xB6;
        static constexpr uintptr_t RT_DRAW_BEGIN_OFFSET = 0xC0;
        static constexpr uintptr_t RT_DRAW_TEX_COORD_OFFSET = 0xCD;
        static constexpr uintptr_t RT_DRAW_VERTEX_OFFSET = 0xDB;
        static constexpr uintptr_t RT_DRAW_END_OFFSET = 0x17A;
        static constexpr uintptr_t RT_DRAW_POP_STATE_OFFSET = 0x1AD;
    };

    // Detect build information
    std::optional<std::string> detect_build();

    // Resolve game function addresses using patterns and fallbacks
    bool resolve_addresses(GameAddresses& out, const core::EngineConfig& cfg);

} // namespace iee::game
