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

        bool enableShaderTracing = false;
        bool enableSpriteTcScaleInjection = false;
        bool enableBatchHighlightProbe = false;
        bool enableBatchSuppressProbe = false;
        bool enableGlProgramSuppressProbe = false;
        bool enableGlTextureSuppressProbe = false;
        int batchHighlightProgram = 0;
        int batchHighlightTextureId = -1;
        int batchSuppressMinScreenWidth = 0;
        int batchSuppressMaxScreenWidth = 0;
        int batchSuppressMinScreenHeight = 0;
        int batchSuppressMaxScreenHeight = 0;
        int batchSuppressMinCenterX = -1;
        int batchSuppressMaxCenterX = -1;
        int batchSuppressMinCenterY = -1;
        int batchSuppressMaxCenterY = -1;
        int glProgramSuppress = 0;
        int glTextureSuppressProgram = 0;
        int glTextureSuppressTexture = -1;
        int shaderTraceRuntimeProgramFilter = 0;
        int shaderTraceRuntimeTextureFilter = -1;
        bool enableSpriteBodyFsrPrototype = false;
        int spriteBodyProgram = 3;
        int spriteBodyTexture = 2;
        float spriteBodyInputScale = 0.667f;
        bool spriteBodyEnableRcas = true;
        float spriteBodyRcasSharpness = 0.20f;
        int spriteBodyDebugView = 0;

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
