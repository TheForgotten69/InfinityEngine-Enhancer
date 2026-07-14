#pragma once

#include <cstdint>

namespace iee::probe::uniforms {

struct Locations {
  static constexpr int kUnresolved = -2;

  int time{kUnresolved};
  int enabled{kUnresolved};
  int scroll{kUnresolved};
  int zoom{kUnresolved};
  int viewport{kUnresolved};
  int worldSizeInv{kUnresolved};
  int waterTint{kUnresolved};
  int areaMask{kUnresolved};
  int normalMap{kUnresolved};
  int dudvMap{kUnresolved};
  int foamMap{kUnresolved};
};

struct Snapshot {
  float effectValue{};
  float scrollX{};
  float scrollY{};
  float viewWorldWidth{};
  float viewWorldHeight{};
  float worldWidth{};
  float worldHeight{};
  unsigned feedCount{};
};

void initialize(bool effectEnabled) noexcept;
void reset() noexcept;
void set_time(float secondsSinceStart) noexcept;
void set_effect_enabled(bool enabled) noexcept;
[[nodiscard]] bool effect_enabled() noexcept;
[[nodiscard]] float cycle_debug_effect() noexcept;
void set_world_size(float widthPx, float heightPx) noexcept;
void set_water_tint(float r, float g, float b) noexcept;
void set_view(float scrollX, float scrollY, float viewWorldWidth, float viewWorldHeight) noexcept;
[[nodiscard]] Snapshot snapshot() noexcept;

// The caller owns program classification and location caching. This function
// only resolves missing locations and feeds the currently bound program.
void feed(unsigned program, Locations& locations);

}  // namespace iee::probe::uniforms
