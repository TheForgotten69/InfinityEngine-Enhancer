#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace iee::game {
struct MotionPoint {
  std::int32_t x{};
  std::int32_t y{};

  friend bool operator==(const MotionPoint&, const MotionPoint&) = default;
};

// Render-rate smoothing of positions that the engine only updates on its
// logic tick. Each key (a live sprite) is drawn sliding from where it was
// last shown to its newest logic position over one observed tick interval,
// so the picture trails the logic by at most one tick.
//
// Host-safe and single-threaded: the caller owns the clock and the thread.
class SpriteMotionTracker {
 public:
  // Logic-tick interval assumed until two consecutive moves are observed.
  static constexpr double kDefaultInterval = 1.0 / 30.0;
  // Observed intervals outside this range are not a steady walk (first step
  // after standing still, a hitch) and fall back to the default.
  static constexpr double kMinInterval = 1.0 / 90.0;
  static constexpr double kMaxInterval = 1.0 / 14.0;
  // A larger single-tick move is a teleport or a reused object address and
  // is shown immediately.
  static constexpr std::int32_t kSnapDistance = 48;
  static constexpr std::size_t kPruneThreshold = 1024;
  static constexpr double kStaleSeconds = 5.0;

  // `current` is the engine's position for `key` at monotonic time `now`
  // (seconds). Returns the position to draw at. Repeated calls with the same
  // arguments return the same result.
  [[nodiscard]] MotionPoint sample(const void* key, MotionPoint current, double now);

  void clear() noexcept { states_.clear(); }
  [[nodiscard]] std::size_t size() const noexcept { return states_.size(); }

 private:
  struct State {
    MotionPoint from{};
    MotionPoint to{};
    double changedAt{};
    double interval{kDefaultInterval};
    double lastSeen{};
  };

  [[nodiscard]] static MotionPoint shown(const State& state, double now) noexcept;
  void prune(double now);

  std::unordered_map<const void*, State> states_;
};
}  // namespace iee::game
