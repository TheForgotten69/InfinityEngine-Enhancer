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

  bool samplersInitialized{};
  bool viewInitialized{};
  bool worldSizeInitialized{};
  bool waterTintInitialized{};
  float lastScrollX{};
  float lastScrollY{};
  float lastViewWorldWidth{};
  float lastViewWorldHeight{};
  float lastWorldWidth{};
  float lastWorldHeight{};
  float lastWaterTintR{};
  float lastWaterTintG{};
  float lastWaterTintB{};
  int lastViewportWidth{};
  int lastViewportHeight{};
  std::uint64_t lastAppliedRevision{};
};

struct FeedPerformanceStats {
  std::uint64_t calls{};
  std::uint64_t skippedUnchanged{};
  std::uint64_t textureBindPasses{};
  std::uint64_t totalTicks{};
  std::uint64_t maximumTicks{};
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

void initialize(bool effectEnabled, bool performanceEnabled) noexcept;
void reset() noexcept;
void set_time(float secondsSinceStart) noexcept;
void set_effect_enabled(bool enabled) noexcept;
[[nodiscard]] bool effect_enabled() noexcept;
[[nodiscard]] float cycle_debug_effect() noexcept;
void set_world_size(float widthPx, float heightPx) noexcept;
void set_water_tint(float r, float g, float b) noexcept;
void set_view(float scrollX, float scrollY, float viewWorldWidth, float viewWorldHeight) noexcept;
[[nodiscard]] Snapshot snapshot() noexcept;
[[nodiscard]] FeedPerformanceStats take_performance_stats() noexcept;

// The caller owns program classification and location caching. This function
// only resolves missing locations and feeds the currently bound program.
void feed(unsigned program, Locations& locations);

}  // namespace iee::probe::uniforms
