#include "tile_upscale.h"

#include <algorithm>

namespace iee::game {
    namespace {
        constexpr std::uint32_t kMaxTableSamples = 32;
    }

    std::optional<ScaleDetectionResult> detect_scale_from_tis_header(const TileInfo &tileInfo,
                                                                     const BuildManifest &manifest) {
        const auto headerTileDimension = get_tis_header_tile_dimension(tileInfo, manifest);
        if (!headerTileDimension) {
            return std::nullopt;
        }

        if (*headerTileDimension == TisTileDimensions::Standard) {
            return ScaleDetectionResult{1, ScaleDetectionSource::TisHeader, *headerTileDimension};
        }

        if (*headerTileDimension == TisTileDimensions::Upscaled4x) {
            return ScaleDetectionResult{4, ScaleDetectionSource::TisHeader, *headerTileDimension};
        }

        return std::nullopt;
    }

    std::optional<ScaleDetectionResult> detect_scale_from_tile_table(const TileInfo &tileInfo) {
        if (!tileInfo.table || tileInfo.runtimeTileDimension == 0) {
            return std::nullopt;
        }

        const auto sampleCount = std::min<std::uint32_t>(tileInfo.runtimeTileDimension, kMaxTableSamples);
        std::uint32_t smallestPositiveStep = 0;

        for (std::uint32_t i = 0; i < sampleCount; ++i) {
            const auto &entry = tileInfo.table[i];

            const auto consider = [&](std::int32_t candidate) {
                if (candidate <= 0) {
                    return;
                }

                const auto value = static_cast<std::uint32_t>(candidate);
                if (smallestPositiveStep == 0 || value < smallestPositiveStep) {
                    smallestPositiveStep = value;
                }
            };

            consider(entry.u);
            consider(entry.v);
        }

        if (smallestPositiveStep == TisTileDimensions::Standard) {
            return ScaleDetectionResult{1, ScaleDetectionSource::TileTable, smallestPositiveStep};
        }

        if (smallestPositiveStep == TisTileDimensions::Upscaled4x) {
            return ScaleDetectionResult{4, ScaleDetectionSource::TileTable, smallestPositiveStep};
        }

        return std::nullopt;
    }

    bool is_upscaled_by_heuristics(const TileInfo &tileInfo, int textureId) {
        if (!tileInfo.table || tileInfo.index < 0) {
            return false;
        }

        const auto &entry = tileInfo.table[tileInfo.index];
        return entry.u > UpscaleThresholds::UV_THRESHOLD ||
               entry.v > UpscaleThresholds::UV_THRESHOLD ||
               textureId > UpscaleThresholds::TEXTURE_ID_THRESHOLD;
    }

    std::optional<ScaleDetectionResult> detect_preferred_scale_hint(const TileInfo &tileInfo,
                                                                    int textureId,
                                                                    const BuildManifest &manifest) {
        if (auto headerDetection = detect_scale_from_tis_header(tileInfo, manifest)) {
            return headerDetection;
        }

        if (auto tableDetection = detect_scale_from_tile_table(tileInfo)) {
            return tableDetection;
        }

        if (is_upscaled_by_heuristics(tileInfo, textureId)) {
            return ScaleDetectionResult{4, ScaleDetectionSource::Heuristic, 0};
        }

        return std::nullopt;
    }
}
