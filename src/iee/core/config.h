#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace iee::core {
    struct EngineConfig {
        // region Textures
        bool enableAnisotropicFiltering = true;
        float maxAnisotropy = 8.0f;
        float lodBias = -0.25f;

        // endregion

        // region Cached addresses (auto-generated from pattern scanning)
        std::uintptr_t cachedLoadAreaRVA = 0x27E710;
        std::uintptr_t cachedRenderTextureRVA = 0x4247E0;

        //endregion

        bool enableVerboseLogging = false;
    };

    class ConfigManager {
    public:
        static std::filesystem::path config_path();

        static bool load(const std::filesystem::path &path, EngineConfig &out);

        static bool save(const std::filesystem::path &path, const EngineConfig &cfg);

        static EngineConfig load_or_default();
    };
}
