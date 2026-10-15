#pragma once

#include <cstdint>
#include <optional>

#include "tis_runtime.h"

namespace iee::game {
namespace UpscaleThresholds {
constexpr int DETECTION_SAMPLE_COUNT = 10;
}  // namespace UpscaleThresholds

namespace TisTileDimensions {
constexpr std::uint32_t Standard = 0x40;
constexpr std::uint32_t Upscaled2x = 0x80;
constexpr std::uint32_t Upscaled4x = 0x100;
constexpr std::uint32_t Upscaled8x = 0x200;
}  // namespace TisTileDimensions

enum class ScaleDetectionSource : std::uint8_t {
  TisHeader,
  TileTable,
};

struct ScaleDetectionResult {
  int scaleFactor{};
  ScaleDetectionSource source{ScaleDetectionSource::TisHeader};
  std::uint32_t detectedTileDimension{};
};

// Supported authored dimensions are power-of-two multiples of the engine's
// fixed 64px screen tile. Keeping an explicit upper bound prevents malformed
// headers from producing unbounded UV spans.
[[nodiscard]] std::optional<int> scale_factor_from_tile_dimension(
    std::uint32_t tileDimension) noexcept;

[[nodiscard]] std::optional<ScaleDetectionResult> detect_scale_from_tis_header(
    const TileInfo& tileInfo, const BuildManifest& manifest);

[[nodiscard]] std::optional<ScaleDetectionResult> infer_scale_from_tile_table(
    const TileInfo& tileInfo);

// Production scale selection: deterministic metadata only (TIS header, then
// PVR entry-table coordinate grid). When neither resolves, nullopt fails
// closed into the caller's sampling path, which delegates the tileset to the
// engine as standard 1x. Raw UV magnitudes and GL texture ids are not valid
// scale signals (atlas origins are arbitrary; texture ids grow with session
// allocations) — the former heuristic built on them produced false 4x
// detections on vanilla areas and was removed.
[[nodiscard]] std::optional<ScaleDetectionResult> detect_scale(const TileInfo& tileInfo,
                                                               int textureId,
                                                               const BuildManifest& manifest);
}  // namespace iee::game
