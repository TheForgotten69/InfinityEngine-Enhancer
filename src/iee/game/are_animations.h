#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "resref_runtime.h"

namespace iee::game {
// Classification of an authored ARE ambient animation for point-effect
// placement (graphics roadmap §10.3: the authored classification channel for
// emissive candidates). Water bodies stay on the WED overlay path.
enum class AreaAnimationKind : int {
  None = 0,
  Fire = 1,
  Smoke = 2,      // includes authored fog/mist
  Fountain = 3,
  Light = 4,      // candles, glows, lit night windows
  Water = 5,      // splashes, ripples, waterfalls, scrolling water
  Lava = 6,
  Wildlife = 7,   // fish, flies, butterflies — explicitly no effect
};

[[nodiscard]] std::string_view area_animation_kind_name(AreaAnimationKind kind) noexcept;

// Conservative resref/name prefix classification. Unknown entries return
// None; runtime logging of unclassified resrefs is the mechanism that grows
// the table from real game data.
[[nodiscard]] AreaAnimationKind classify_area_animation(std::string_view resref,
                                                        std::string_view name) noexcept;

struct AreaAnimationInfo {
  std::uint16_t x{};
  std::uint16_t y{};
  std::int16_t height{};
  std::uint32_t schedule{};
  std::uint32_t flags{};
  std::uint16_t translucency{};
  ResrefBuffer resref{};
  // Authored editor label, NUL-terminated (source field is 32 bytes).
  std::array<char, 33> name{};
  AreaAnimationKind kind{AreaAnimationKind::None};

  [[nodiscard]] std::string_view resrefView() const noexcept { return resref_view(resref); }
  [[nodiscard]] std::string_view nameView() const noexcept;
  [[nodiscard]] bool isShown() const noexcept;
  [[nodiscard]] bool isLightSource() const noexcept;
};

struct AreaAnimationsInfo {
  ResrefBuffer areaResref{};
  std::vector<AreaAnimationInfo> animations{};

  [[nodiscard]] std::string_view areaResrefView() const noexcept {
    return resref_view(areaResref);
  }
  [[nodiscard]] std::size_t count_of(AreaAnimationKind kind) const noexcept;
};

// Shared ceiling for authored animation records, whichever channel supplies
// them (disk parse or the in-memory CGameStatic walk).
inline constexpr std::size_t kMaxAreaAnimationRecords = 4096;

struct ARE_Animation_st;

// Converts one raw authored record (identical layout on disk and inside a
// live CGameStatic) into a classified AreaAnimationInfo.
[[nodiscard]] AreaAnimationInfo make_area_animation_info(const ARE_Animation_st& record) noexcept;

// Parses the animation section out of a complete ARE V1.0 file image.
// Bounded and fail-closed: malformed counts/offsets return false and leave
// `out` empty. Other ARE versions (e.g. IWD2 V9.1) are rejected.
[[nodiscard]] bool parse_are_animations(const std::byte* data, std::size_t size,
                                        AreaAnimationsInfo& out) noexcept;

// One shader point effect: world-pixel position, the AreaAnimationKind as a
// float, and a per-kind strength scale. Matches the uIeePoints vec4 layout.
struct AreaEffectPoint {
  float x{};
  float y{};
  float kind{};
  float strength{};
};

// The fpSEAM override's fixed uniform-array capacity.
inline constexpr std::size_t kMaxAreaEffectPoints = 32;

// Selects the shown fire/smoke/light animations as shader points, fire first
// so the capacity cap drops the least impactful kinds. Returns at most
// kMaxAreaEffectPoints entries.
[[nodiscard]] std::vector<AreaEffectPoint> build_area_effect_points(
    const AreaAnimationsInfo& info);
}  // namespace iee::game
