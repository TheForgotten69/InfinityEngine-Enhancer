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
  // Live CGameObject::m_pos — the position RenderBam actually draws from
  // (can differ from the header x/y). Filled by the memory walk; the disk
  // parser mirrors x/y.
  std::int32_t objX{};
  std::int32_t objY{};
  // The engine's cached current BAM frame entry (CVidCell::m_pFrame) — the
  // exact geometry RenderBam draws with. Valid only when the memory walk
  // could read it (the object has rendered at least once).
  bool frameValid{};
  std::int16_t frameWidth{};
  std::int16_t frameHeight{};
  std::int16_t frameCenterX{};
  std::int16_t frameCenterY{};
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

// One shader point effect, two vec4 uniform slots per point.
// Slot A: base-center world position (anchor corrected by the authored BAM
// draw box), the integer kind, and effect height in world px.
// Slot B: half-width in world px, then the palette id (fire only: 0 warm
// body, 1 blue body, 2 glow-only — the engine keeps drawing the authored
// art); the rest is reserved.
struct AreaEffectPoint {
  float x{};
  float y{};
  float kind{};
  float height{};
  float halfWidth{};
  float reserved1{};  // palette id for fire points
  float reserved2{};
  float reserved3{};
};
static_assert(sizeof(AreaEffectPoint) == 8 * sizeof(float));

// The fpSEAM override's fixed capacity (points; the uniform array holds two
// vec4 slots per point).
inline constexpr std::size_t kMaxAreaEffectPoints = 32;

// True when the animation's authored draw is a standalone flame/smoke BAM
// that the shader replaces outright (the CGameStatic::Render hook suppresses
// the engine draw). False for per-area overlay art (hearths, plume images)
// that must keep rendering; fire overlays then contribute glow only.
[[nodiscard]] bool should_replace_animation_draw(std::string_view resref,
                                                 AreaAnimationKind kind) noexcept;

// Selects the shown fire/smoke/light animations as shader points, fire first
// so the capacity cap drops the least impactful kinds. Returns at most
// kMaxAreaEffectPoints entries.
[[nodiscard]] std::vector<AreaEffectPoint> build_area_effect_points(
    const AreaAnimationsInfo& info);
}  // namespace iee::game
