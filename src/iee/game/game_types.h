#pragma once
#include <cstdint>

namespace iee::game {
    enum class DrawMode : int {
        Triangles = 2,
        TriangleStrip = 3
    };

    enum class ShaderTone : int {
        None = 0,
        Grey = 1,
        Seam = 8,
    };

    namespace TileDimensions {
        constexpr int STANDARD_SIZE = 64;
        constexpr int RENDER_QUAD_SIZE = 64;
    }

    namespace RenderFlags {
        constexpr int GREY_TONE_BIT = 19;
        constexpr unsigned long GREY_TONE_MASK = 1UL << GREY_TONE_BIT;
    }
}
