#include "tile_upscale.h"

#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace iee::game {
namespace {
constexpr std::uint32_t kMaxTableSamples = 32;
constexpr std::int32_t kMaxAtlasCoordinate = 16384;
}  // namespace

std::optional<ScaleDetectionResult> detect_scale_from_tis_header(const TileInfo& tileInfo,
                                                                 const BuildManifest& manifest) {
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

std::optional<ScaleDetectionResult> infer_scale_from_tile_table(const TileInfo& tileInfo) {
  if (!tileInfo.table || tileInfo.tileCount == 0) {
    return std::nullopt;
  }

  const auto sampleCount = std::min<std::uint32_t>(tileInfo.tileCount, kMaxTableSamples);
  std::vector<PVRZTileEntry> entries;
  entries.reserve(sampleCount);

  for (std::uint32_t i = 0; i < sampleCount; ++i) {
    PVRZTileEntry entry{};
    if (!read_tis_tile_entry(tileInfo, i, entry)) {
      return std::nullopt;
    }
    if (entry.page < 0 || entry.u < 0 || entry.v < 0 || entry.u > kMaxAtlasCoordinate ||
        entry.v > kMaxAtlasCoordinate) {
      return std::nullopt;
    }
    entries.push_back(entry);
  }

  // Atlas origins are arbitrary; raw U/V values are not tile dimensions.
  // The GCD of same-page coordinate deltas is translation-invariant and
  // resolves the authored grid only when the sampled table is unambiguous.
  std::uint32_t coordinateStep = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    for (std::size_t j = i + 1; j < entries.size(); ++j) {
      if (entries[i].page != entries[j].page) continue;
      const auto du = static_cast<std::uint32_t>(std::abs(entries[i].u - entries[j].u));
      const auto dv = static_cast<std::uint32_t>(std::abs(entries[i].v - entries[j].v));
      if (du != 0) coordinateStep = std::gcd(coordinateStep, du);
      if (dv != 0) coordinateStep = std::gcd(coordinateStep, dv);
    }
  }

  if (coordinateStep == TisTileDimensions::Standard) {
    return ScaleDetectionResult{1, ScaleDetectionSource::TileTable, coordinateStep};
  }

  if (coordinateStep == TisTileDimensions::Upscaled4x) {
    return ScaleDetectionResult{4, ScaleDetectionSource::TileTable, coordinateStep};
  }

  return std::nullopt;
}

bool is_upscaled_by_heuristics(const TileInfo& tileInfo, int textureId) {
  if (!tileInfo.table || tileInfo.index < 0 ||
      static_cast<std::uint32_t>(tileInfo.index) >= tileInfo.tileCount) {
    return false;
  }

  const auto& entry = tileInfo.entry;
  return entry.u > UpscaleThresholds::UV_THRESHOLD || entry.v > UpscaleThresholds::UV_THRESHOLD ||
         textureId > UpscaleThresholds::TEXTURE_ID_THRESHOLD;
}

std::optional<ScaleDetectionResult> detect_scale(const TileInfo& tileInfo, int textureId,
                                                 const BuildManifest& manifest) {
  if (auto headerDetection = detect_scale_from_tis_header(tileInfo, manifest)) {
    return headerDetection;
  }

  if (auto tableDetection = infer_scale_from_tile_table(tileInfo)) {
    return tableDetection;
  }

  if (is_upscaled_by_heuristics(tileInfo, textureId)) {
    return ScaleDetectionResult{4, ScaleDetectionSource::Heuristic, 0};
  }

  return std::nullopt;
}
}  // namespace iee::game
