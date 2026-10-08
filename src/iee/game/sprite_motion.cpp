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

  const bool forgotten = now - state.lastSeen > kForgetSeconds;
  state.lastSeen = now;
  if (forgotten) {
    state = {current, current, now, kDefaultInterval, now};
    return current;
  }
  if (current != state.to) {
    const auto jump = (std::max)(std::abs(current.x - state.to.x), std::abs(current.y - state.to.y));
    if (jump > snapDistance_) {
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

void TickClock::on_update(double now) noexcept {
  const double last = lastUpdate_.exchange(now, std::memory_order_relaxed);
  if (last >= 0.0 && now - last < kBurstSeconds && now >= last) return;
  const double previousTick = tickStart_.exchange(now, std::memory_order_relaxed);
  const double observed = now - previousTick;
  interval_.store(previousTick >= 0.0 && observed >= kMinInterval && observed <= kMaxInterval
                      ? observed
                      : kDefaultInterval,
                  std::memory_order_relaxed);
}

double TickClock::phase(double now) const noexcept {
  const double start = tickStart_.load(std::memory_order_relaxed);
  if (start < 0.0) return 1.0;
  const double elapsed = (now - start) / interval_.load(std::memory_order_relaxed);
  // Written so NaN lands on the logic position.
  if (!(elapsed < 1.0)) return 1.0;
  return elapsed > 0.0 ? elapsed : 0.0;
}

ParticlePoint particle_draw_position(const ParticleState& particle, double phase) noexcept {
  if (!(phase >= 0.0) || !(phase < 1.0)) return particle.position;
  const double back = 1.0 - phase;
  const auto step = [back](std::int32_t delta) {
    return static_cast<std::int32_t>(std::lround(static_cast<double>(delta) * back));
  };
  const auto heightStep = particle.gravityOnly ? -particle.gravity : particle.velocity.z;
  return {particle.position.x - step(particle.velocity.x),
          particle.position.y - step(particle.velocity.y), particle.position.z - step(heightStep)};
}
}  // namespace iee::game
