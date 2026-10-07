#pragma once

namespace iee::game {
// Sizing for the world post-processing passes (soft fog of war, bloom). Pure
// arithmetic so it can be tested without a GL context.

struct Extent {
  int width{};
  int height{};
  friend bool operator==(const Extent&, const Extent&) = default;
};

inline constexpr int kMaxBlurLevels = 6;

// A dual-filter blur: `levels` successive half-size images below the source,
// sampled with `offset` texels of spread. levels == 0 means "do not blur".
struct BlurPlan {
  int levels{};
  float offset{1.0f};
};

Extent half_extent(Extent extent) noexcept;
// How many half-size levels fit below `base` before a side drops under 8 px.
int available_blur_levels(Extent base) noexcept;
// The plan whose blur reaches about `radiusPixels` on an image of `base` size.
BlurPlan blur_plan_for_radius(float radiusPixels, Extent base) noexcept;
int bloom_levels(Extent base) noexcept;
// Screen pixels per world pixel at the current zoom; 1 when either is unknown.
float pixels_per_world_pixel(float viewportWidth, float viewWorldWidth) noexcept;
}  // namespace iee::game
