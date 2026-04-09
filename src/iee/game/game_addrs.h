#pragma once
#include <cstdint>

namespace iee {
    namespace core { struct EngineConfig; }
}

namespace iee::game {
    struct BuildManifest;

    struct GameAddresses {
        std::uintptr_t LoadArea = 0;
        std::uintptr_t RenderTexture = 0;
        bool initialized = false;
    };

    bool resolve_addresses(GameAddresses &out, const core::EngineConfig &cfg, const BuildManifest &manifest);

} // namespace iee::game
