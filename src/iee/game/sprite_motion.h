#pragma once

#include <atomic>
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
  explicit SpriteMotionTracker(std::int32_t snapDistance = kSnapDistance) noexcept
      : snapDistance_(snapDistance) {}

  // Logic-tick interval assumed until two consecutive moves are observed.
  static constexpr double kDefaultInterval = 1.0 / 30.0;
  // Observed intervals outside this range are not a steady walk (first step
  // after standing still, a hitch) and fall back to the default.
  static constexpr double kMinInterval = 1.0 / 90.0;
  static constexpr double kMaxInterval = 1.0 / 14.0;
  // A larger single-tick move is a teleport or a reused object address and
  // is shown immediately. Creatures use the default; fast movers (projectiles)
  // pass a larger limit.
  static constexpr std::int32_t kSnapDistance = 48;
  // A key not sampled for this long was off screen or is a new object at a
  // reused address: it starts over instead of sliding from a stale position.
  static constexpr double kForgetSeconds = 0.5;
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

  std::int32_t snapDistance_;
  std::unordered_map<const void*, State> states_;
};

// The engine's logic tick, as seen from any function it calls once (or many
// times in a burst) per tick. Things the engine advances by a known amount per
// tick can then be drawn part of the way through the step. on_update may run
// on a different thread from phase.
class TickClock {
 public:
  static constexpr double kDefaultInterval = 1.0 / 30.0;
  static constexpr double kMinInterval = 1.0 / 90.0;
  static constexpr double kMaxInterval = 1.0 / 14.0;
  // Calls closer together than this belong to the same tick.
  static constexpr double kBurstSeconds = 0.004;

  void on_update(double now) noexcept;
  // 0 right after a tick, rising to 1 one tick interval later and staying
  // there (so nothing drifts while the game is paused). 1 before any tick.
  [[nodiscard]] double phase(double now) const noexcept;

 private:
  std::atomic<double> tickStart_{-1.0};
  std::atomic<double> lastUpdate_{-1.0};
  std::atomic<double> interval_{kDefaultInterval};
};

struct ParticlePoint {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t z{};
  friend bool operator==(const ParticlePoint&, const ParticlePoint&) = default;
};

// A CParticle after its per-tick update: the engine added `velocity` to the
// position (CParticle::AsynchronousUpdate). A gravity-only particle instead
// lost `gravity` in height.
struct ParticleState {
  ParticlePoint position{};
  ParticlePoint velocity{};
  std::int32_t gravity{};
  bool gravityOnly{};
};

// Where to draw a particle `phase` of the way through the current tick: its
// previous position at 0, its logic position at 1.
ParticlePoint particle_draw_position(const ParticleState& particle, double phase) noexcept;
}  // namespace iee::game
