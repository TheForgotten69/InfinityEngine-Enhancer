#include "iee/game/world_post_plan.h"

#include <algorithm>
#include <cmath>

namespace iee::game {
namespace {
constexpr int kMinBlurExtent = 8;
constexpr int kMaxBloomLevels = 5;
}  // namespace

Extent half_extent(Extent extent) noexcept {
  return {std::max(1, extent.width / 2), std::max(1, extent.height / 2)};
}

int available_blur_levels(Extent base) noexcept {
  int levels = 0;
  Extent extent = base;
  while (levels < kMaxBlurLevels) {
    extent = half_extent(extent);
    if (std::min(extent.width, extent.height) < kMinBlurExtent) break;
    ++levels;
  }
  return levels;
}

BlurPlan blur_plan_for_radius(float radiusPixels, Extent base) noexcept {
  // Written so NaN fails the test.
  if (!(radiusPixels >= 1.0f)) return {};
  const int available = available_blur_levels(base);
  if (available == 0) return {};
  int levels = 1;
  while (levels < available && static_cast<float>(1 << (levels + 1)) < radiusPixels) ++levels;
  const auto reach = static_cast<float>(1 << (levels + 1));
  return {levels, std::clamp(radiusPixels / reach, 0.5f, 1.5f)};
}

int bloom_levels(Extent base) noexcept {
  return std::min(kMaxBloomLevels, available_blur_levels(base));
}

float pixels_per_world_pixel(float viewportWidth, float viewWorldWidth) noexcept {
  if (!std::isfinite(viewportWidth) || !std::isfinite(viewWorldWidth)) return 1.0f;
  if (!(viewportWidth > 0.0f) || !(viewWorldWidth > 0.0f)) return 1.0f;
  return std::clamp(viewportWidth / viewWorldWidth, 0.25f, 8.0f);
}
}  // namespace iee::game
