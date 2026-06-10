#include "tile_liquid_shader.h"

#include <string>

namespace iee::game {
    namespace {
        constexpr std::string_view kPatchMarker = "IEE_TILE_LIQUID_PATCH";

        bool contains(std::string_view haystack, std::string_view needle) noexcept {
            return haystack.find(needle) != std::string_view::npos;
        }

        std::size_t find_main_signature(std::string_view source) noexcept {
            constexpr std::string_view kPatterns[] = {
                "void main(",
                "void main (",
            };

            for (const auto pattern: kPatterns) {
                if (const auto pos = source.find(pattern); pos != std::string_view::npos) {
                    return pos;
                }
            }

            return std::string_view::npos;
        }

        bool find_main_body_bounds(std::string_view source,
                                   std::size_t &outMainPos,
                                   std::size_t &outBodyOpenPos,
                                   std::size_t &outBodyClosePos) noexcept {
            outMainPos = find_main_signature(source);
            if (outMainPos == std::string_view::npos) {
                return false;
            }

            outBodyOpenPos = source.find('{', outMainPos);
            if (outBodyOpenPos == std::string_view::npos) {
                return false;
            }

            int depth = 0;
            for (std::size_t pos = outBodyOpenPos; pos < source.size(); ++pos) {
                const auto ch = source[pos];
                if (ch == '{') {
                    ++depth;
                } else if (ch == '}') {
                    --depth;
                    if (depth == 0) {
                        outBodyClosePos = pos;
                        return true;
                    }
                }
            }

            return false;
        }

        constexpr char kInjectedPrelude[] = R"GLSL(

/* IEE_TILE_LIQUID_PATCH */
uniform float uIeeTileLiquidMode;
uniform float uIeeLiquidTime;
uniform float uIeeTileUvU0;
uniform float uIeeTileUvV0;
uniform float uIeeTileUvDU;
uniform float uIeeTileUvDV;
uniform float uIeeTileMaskRow0;
uniform float uIeeTileMaskRow1;
uniform float uIeeTileMaskRow2;
uniform float uIeeTileMaskRow3;
uniform float uIeeTileMaskRow4;
uniform float uIeeTileMaskRow5;
uniform float uIeeTileMaskRow6;
uniform float uIeeTileMaskRow7;

float ieeTileLiquidMaskRow(float rowIndex)
{
    if (rowIndex < 0.5) return uIeeTileMaskRow0;
    if (rowIndex < 1.5) return uIeeTileMaskRow1;
    if (rowIndex < 2.5) return uIeeTileMaskRow2;
    if (rowIndex < 3.5) return uIeeTileMaskRow3;
    if (rowIndex < 4.5) return uIeeTileMaskRow4;
    if (rowIndex < 5.5) return uIeeTileMaskRow5;
    if (rowIndex < 6.5) return uIeeTileMaskRow6;
    return uIeeTileMaskRow7;
}

float ieeTileLiquidMaskBit(vec2 localUv)
{
    float x = floor(clamp(localUv.x, 0.0, 0.99999) * 8.0);
    float y = floor(clamp(localUv.y, 0.0, 0.99999) * 8.0);
    float rowMask = ieeTileLiquidMaskRow(y);
    return mod(floor(rowMask / exp2(x)), 2.0);
}

vec3 ieeTileLiquidDeepColor(float mode)
{
    if (mode < 1.5) return vec3(0.05, 0.26, 0.43);
    if (mode < 2.5) return vec3(0.40, 0.12, 0.02);
    if (mode < 3.5) return vec3(0.12, 0.28, 0.08);
    if (mode < 4.5) return vec3(0.30, 0.28, 0.08);
    return vec3(0.16, 0.26, 0.10);
}

vec3 ieeTileLiquidShallowColor(float mode)
{
    if (mode < 1.5) return vec3(0.40, 0.77, 0.95);
    if (mode < 2.5) return vec3(1.00, 0.60, 0.16);
    if (mode < 3.5) return vec3(0.62, 0.86, 0.32);
    if (mode < 4.5) return vec3(0.78, 0.76, 0.30);
    return vec3(0.55, 0.78, 0.34);
}

vec4 ieeApplyTileLiquid(vec4 currentColor)
{
    if (uIeeTileLiquidMode < 0.5) return currentColor;

    vec2 uvOrigin = vec2(uIeeTileUvU0, uIeeTileUvV0) * uTcScale;
    vec2 uvSize = max(vec2(uIeeTileUvDU, uIeeTileUvDV) * uTcScale, vec2(0.000001));
    vec2 localUv = clamp((vTc - uvOrigin) / uvSize, vec2(0.0), vec2(0.99999));
    if (ieeTileLiquidMaskBit(localUv) < 0.5) return currentColor;

    float time = uIeeLiquidTime;
    float wave0 = sin(localUv.x * 18.0 + localUv.y * 11.0 + time * 1.20);
    float wave1 = cos(localUv.x * 9.0 - localUv.y * 15.0 - time * 0.85);
    float wave2 = sin((localUv.x + localUv.y) * 24.0 + time * 0.55);
    float ripple = clamp(0.5 + 0.5 * (0.55 * wave0 + 0.30 * wave1 + 0.15 * wave2), 0.0, 1.0);
    float foam = smoothstep(0.80, 0.98, ripple);

    vec3 rgb = mix(ieeTileLiquidDeepColor(uIeeTileLiquidMode),
                   ieeTileLiquidShallowColor(uIeeTileLiquidMode),
                   ripple);
    rgb = mix(rgb, vec3(1.0), foam * 0.20);
    return vec4(rgb, currentColor.a);
}
)GLSL";

        constexpr char kInjectedMainTail[] = R"GLSL(
    gl_FragColor = ieeApplyTileLiquid(gl_FragColor);
)GLSL";
    }

    TileLiquidShaderPatchResult patch_fpseam_liquid_fragment_shader(std::string_view source) {
        TileLiquidShaderPatchResult result{};
        result.source = std::string(source);

        if (source.empty() || contains(source, kPatchMarker)) {
            return result;
        }

        if (!contains(source, "uTcScale") ||
            !contains(source, "vTc")) {
            return result;
        }

        std::size_t mainPos = std::string_view::npos;
        std::size_t bodyOpenPos = std::string_view::npos;
        std::size_t bodyClosePos = std::string_view::npos;
        if (!find_main_body_bounds(source, mainPos, bodyOpenPos, bodyClosePos)) {
            return result;
        }

        result.source.insert(mainPos, kInjectedPrelude);
        result.source.insert(bodyClosePos + std::char_traits<char>::length(kInjectedPrelude), kInjectedMainTail);
        result.patched = true;
        return result;
    }
}
