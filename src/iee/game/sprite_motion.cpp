#include "sprite_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace iee::game {
MotionPoint SpriteMotionTracker::shown(const State& state, double now) noexcept {
  const double progress = std::clamp((now - state.changedAt) / state.interval, 0.0, 1.0);
  const auto mix = [progress](std::int32_t from, std::int32_t to) {
    return static_cast<std::int32_t>(std::lround(from + (to - from) * progress));
  };
  return {mix(state.from.x, state.to.x), mix(state.from.y, state.to.y)};
}

void SpriteMotionTracker::prune(double now) {
  for (auto it = states_.begin(); it != states_.end();) {
    it = now - it->second.lastSeen > kStaleSeconds ? states_.erase(it) : std::next(it);
  }
}

MotionPoint SpriteMotionTracker::sample(const void* key, MotionPoint current, double now) {
  auto [it, inserted] = states_.try_emplace(key);
  auto& state = it->second;
  if (inserted) {
    state = {current, current, now, kDefaultInterval, now};
    if (states_.size() > kPruneThreshold) prune(now);
    return current;
  }

  state.lastSeen = now;
  if (current != state.to) {
    const auto jump = (std::max)(std::abs(current.x - state.to.x), std::abs(current.y - state.to.y));
    if (jump > kSnapDistance) {
      state.from = current;
    } else {
      // Start from what is on screen now so an early tick never jumps back.
      state.from = shown(state, now);
      const double observed = now - state.changedAt;
      state.interval =
          observed >= kMinInterval && observed <= kMaxInterval ? observed : kDefaultInterval;
    }
    state.to = current;
    state.changedAt = now;
  }
  return shown(state, now);
}
}  // namespace iee::game
