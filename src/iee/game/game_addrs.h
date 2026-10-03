#pragma once
#include <array>
#include <cstdint>

namespace iee {
    namespace core { struct EngineConfig; }
}

namespace iee::game {
    struct BuildManifest;

    struct GameAddresses {
        std::uintptr_t LoadArea = 0;
        std::uintptr_t RenderTexture = 0;
        // Optional (0 when the pattern did not resolve uniquely): the point
        // effects then stay additive instead of replacing the engine draws.
        std::uintptr_t StaticRender = 0;
        // Optional, all-or-nothing: sprite movement smoothing needs every
        // render that reads the sprite position.
        std::uintptr_t SpriteRender = 0;
        std::uintptr_t SpriteRenderMarkers = 0;
        std::uintptr_t SpriteRenderHealthBar = 0;
        // Optional, all-or-nothing: draw-time animation interpolation.
        std::uintptr_t VidCellGetFrame = 0;
        std::uintptr_t VidCellGetCurrentCenterPoint = 0;
        std::uintptr_t VidCellGetCurrentFrameSize = 0;
        // Optional, each independent; 0 where unresolved. Indexed like
        // BuildManifest::smoothedObjectRenders.
        std::array<std::uintptr_t, 8> SmoothedObjectRenders{};
        bool initialized = false;
    };

    bool resolve_addresses(GameAddresses &out, const core::EngineConfig &cfg, const BuildManifest &manifest);

} // namespace iee::game
