#pragma once
#include <array>
#include <cstdint>

#include "iee/game/build_manifest.h"

namespace iee {
    namespace core { struct EngineConfig; }
}

namespace iee::game {

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
        std::array<std::uintptr_t, kMaxSmoothedObjectRenders> SmoothedObjectRenders{};
        // Optional pair: particle (rain, snow, sparkle) movement smoothing.
        std::uintptr_t ParticleUpdate = 0;
        std::uintptr_t ParticleRender = 0;
        // Optional, all-or-nothing: the world post passes (soft fog of war,
        // bloom) need the fog render to hook and the queue flush to call.
        std::uintptr_t RenderFog = 0;
        std::uintptr_t DrawFlush = 0;
        // Optional: the draw queue DrawFlush_GL consumes (count and command
        // array). 0 leaves the fog and flush hooks working without bloom.
        std::uintptr_t DrawQueueCount = 0;
        std::uintptr_t DrawQueueCommands = 0;
        // Optional: the sprite atlas upload (TexSubImage_GL), for sprite
        // upscaling. Needs the draw queue.
        std::uintptr_t TextureUpload = 0;
        bool initialized = false;
    };

    bool resolve_addresses(GameAddresses &out, const core::EngineConfig &cfg, const BuildManifest &manifest);

} // namespace iee::game
