#pragma once

#include <cstdint>
#include <optional>

#include "tis_runtime.h"

namespace iee::game {
    namespace UpscaleThresholds {
        constexpr int UV_THRESHOLD = 1024;
        constexpr int TEXTURE_ID_THRESHOLD = 10000;
        constexpr int UI_TEXTURE_THRESHOLD = 100;
        constexpr int DETECTION_SAMPLE_COUNT = 10;
    }

    namespace TisTileDimensions {
        constexpr std::uint32_t Standard = 0x40;
        constexpr std::uint32_t Upscaled4x = 0x100;
    }

    enum class ScaleDetectionSource : std::uint8_t {
        TisHeader,
        TileTable,
        Heuristic,
    };

    struct ScaleDetectionResult {
        int scaleFactor{};
        ScaleDetectionSource source{ScaleDetectionSource::TisHeader};
        std::uint32_t detectedTileDimension{};
    };

    [[nodiscard]] std::optional<ScaleDetectionResult> detect_scale_from_tis_header(const TileInfo &tileInfo,
                                                                                   const BuildManifest &manifest);

    [[nodiscard]] std::optional<ScaleDetectionResult> detect_scale_from_tile_table(const TileInfo &tileInfo);

    [[nodiscard]] bool is_upscaled_by_heuristics(const TileInfo &tileInfo, int textureId);

    [[nodiscard]] std::optional<ScaleDetectionResult> detect_preferred_scale_hint(const TileInfo &tileInfo,
                                                                                  int textureId,
                                                                                  const BuildManifest &manifest);
}
