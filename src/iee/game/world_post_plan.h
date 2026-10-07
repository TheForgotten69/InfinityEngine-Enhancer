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
// Tells world draws from UI draws by where a frame is. Within a frame that
// shows an area, everything queued up to the fog of war is world and everything
// after it is UI; a frame that shows no area (menus, full-screen panels) is
// all UI. The frame boundary may be reported more than once per presented
// frame, so "an area was drawn recently" is counted in boundaries, not exact.
class WorldPassGate {
 public:
  void on_frame() noexcept {
    if (sinceArea_ < kNever) ++sinceArea_;
    closedForFrame_ = false;
  }
  // The fog of war is about to be drawn: the world part of this frame is over.
  void on_area_drawn() noexcept {
    sinceArea_ = 0;
    closedForFrame_ = true;
  }
  [[nodiscard]] bool open() const noexcept {
    return !closedForFrame_ && sinceArea_ <= kTolerance;
  }

 private:
  static constexpr int kTolerance = 4;
  static constexpr int kNever = 1000;
  int sinceArea_{kNever};
  bool closedForFrame_{};
};

// How far to move the shown image towards the new one after `stepSeconds`,
// for an exponential approach with time constant `smoothingSeconds`. 1 means
// "show the new image": no smoothing, or a history too old to trust.
float temporal_blend(float stepSeconds, float smoothingSeconds) noexcept;
// Screen pixels per world pixel at the current zoom; 1 when either is unknown.
float pixels_per_world_pixel(float viewportWidth, float viewWorldWidth) noexcept;
}  // namespace iee::game
