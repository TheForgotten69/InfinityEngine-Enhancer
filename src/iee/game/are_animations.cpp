#include "are_animations.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>

#include "file_formats.h"

namespace iee::game {
namespace {
constexpr std::uint32_t kAreFileType = 0x41455241;     // "AREA"
constexpr std::uint32_t kAreFileVersion = 0x302E3156;  // "V1.0"

template <typename T>
bool read_struct(const std::byte* base, std::size_t size, std::size_t offset, T& out) noexcept {
  if (offset > size || size - offset < sizeof(T)) {
    return false;
  }

  std::memcpy(&out, base + offset, sizeof(T));
  return true;
}

ResrefBuffer copy_resref(const std::array<std::uint8_t, 8>& raw) noexcept {
  ResrefBuffer out{};
  out.fill('\0');
  for (std::size_t i = 0; i < 8; ++i) {
    const auto c = static_cast<char>(raw[i]);
    if (c == '\0') break;
    out[i] = c;
  }
  return out;
}

std::string upper_copy(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return result;
}

bool starts_with_any(std::string_view value,
                     std::initializer_list<std::string_view> prefixes) noexcept {
  for (const auto prefix : prefixes) {
    if (value.starts_with(prefix)) return true;
  }
  return false;
}

bool contains_any(std::string_view value, std::initializer_list<std::string_view> needles) {
  for (const auto needle : needles) {
    if (value.find(needle) != std::string_view::npos) return true;
  }
  return false;
}
}  // namespace

std::string_view area_animation_kind_name(AreaAnimationKind kind) noexcept {
  switch (kind) {
    case AreaAnimationKind::Fire:
      return "fire";
    case AreaAnimationKind::Smoke:
      return "smoke";
    case AreaAnimationKind::Fountain:
      return "fountain";
    case AreaAnimationKind::Light:
      return "light";
    case AreaAnimationKind::Water:
      return "water";
    case AreaAnimationKind::Lava:
      return "lava";
    case AreaAnimationKind::Wildlife:
      return "wildlife";
    case AreaAnimationKind::None:
    default:
      return "none";
  }
}

namespace {
// Per-area overlays whose kind was established by visual frame inspection of
// the exported BAMs (see docs/are-animation-detection.md). Exact matches win
// over every generic rule.
struct ExactResrefKind {
  std::string_view resref;
  AreaAnimationKind kind;
};
constexpr ExactResrefKind kExactResrefKinds[] = {
    {"AM003XA", AreaAnimationKind::Fire},      // lit hearth overlay (many BG2 city areas)
    {"AM4000Z", AreaAnimationKind::Fire},      // flame columns (AR4000 heads)
    {"AM5204D", AreaAnimationKind::Fire},      // bonfire
    {"AM5508D", AreaAnimationKind::Fire},      // ember glow arc
    {"AM5508C", AreaAnimationKind::Light},     // glow orb
    {"AM0202FL", AreaAnimationKind::Light},    // star glint/flare
    {"AM6004A", AreaAnimationKind::Smoke},     // dark smoke plume
    {"AM6004B", AreaAnimationKind::Smoke},     // dark smoke plume
    {"AM0604A", AreaAnimationKind::Fountain},  // tiered fountain
};
}  // namespace

// Table grown from the full BG1EE + BG2EE ARE exports plus visual frame
// inspection of the ambiguous BAMs. Candle-named flames are deliberately
// Light (dim glow), bare FLM*/FLSM* flames are Fire. Note: AR<area>W[DN]*
// overlays are day/night SHADOW scenery, not lit windows — verified from
// frames; they intentionally stay None.
AreaAnimationKind classify_area_animation(std::string_view resref,
                                          std::string_view name) noexcept {
  if (resref.empty() && name.empty()) return AreaAnimationKind::None;

  const auto upperResref = upper_copy(resref);
  const auto upperName = upper_copy(name);

  for (const auto& exact : kExactResrefKinds) {
    if (upperResref == exact.resref) return exact.kind;
  }

  if (upperName.find("CANDLE") != std::string_view::npos) {
    return AreaAnimationKind::Light;
  }
  // "FPIT" (fire pit) confirmed against BG2EE AR0406; FLM*/FLSM* are the
  // BG1EE small/medium/large flame BAM families (sconces, wall flames);
  // FIM* are yellow flames (OH areas) and YSFL* the blue flames of AR1009 —
  // both verified from exported frames.
  if (starts_with_any(upperResref,
                      {"FLAM", "FIRE", "TORCH", "BRAZ", "FPIT", "FLM", "FLSM", "FIM", "YSFL"}) ||
      contains_any(upperName, {"FIRE", "FLAME", "TORCH", "BRAZIER", "SCONCE"})) {
    return AreaAnimationKind::Fire;
  }
  // Steam pipes (BG2EE AR3017 planar machinery, sewers) read as smoke.
  if (starts_with_any(upperResref, {"SMOK", "CHIM", "STEAM", "AMSTEAM"}) ||
      contains_any(upperName, {"SMOKE", "CHIMNEY", "FOG", "MIST", "STEAM"})) {
    return AreaAnimationKind::Smoke;
  }
  if (starts_with_any(upperResref, {"FOUNT", "FNTN"}) ||
      contains_any(upperName, {"FOUNTAIN"})) {
    return AreaAnimationKind::Fountain;
  }
  if (contains_any(upperName, {"LAVA"})) {
    return AreaAnimationKind::Lava;
  }
  if (starts_with_any(upperResref, {"SPLASH", "RIPPLE", "WTDBL", "BUBBLE"}) ||
      contains_any(upperName, {"WATERFALL", "WATER", "SPLASH", "RIPPLE", "LAKE", "RIVER",
                               "BUBBLE"})) {
    return AreaAnimationKind::Water;
  }
  if (starts_with_any(upperResref, {"BUTRFLY", "FLIES", "FISH", "BIRD"}) ||
      contains_any(upperName, {"FISH", "FLIES", "BUTTERFLY", "BUTRFLY", "MAGGOT"})) {
    return AreaAnimationKind::Wildlife;
  }
  // "LIGHTNING" is weather, not an authored light source.
  const bool nameIsLightning = upperName.find("LIGHTNING") != std::string_view::npos;
  if (starts_with_any(upperResref, {"GLOW", "CANDL", "LANT", "LAMP"}) ||
      (!nameIsLightning && contains_any(upperName, {"GLOW", "LANTERN", "LAMP", "LIGHT"}))) {
    return AreaAnimationKind::Light;
  }
  return AreaAnimationKind::None;
}

std::string_view AreaAnimationInfo::nameView() const noexcept {
  std::size_t length = 0;
  while (length < name.size() - 1 && name[length] != '\0') ++length;
  return {name.data(), length};
}

bool AreaAnimationInfo::isShown() const noexcept {
  return (flags & kAreAnimationFlagIsShown) != 0;
}

bool AreaAnimationInfo::isLightSource() const noexcept {
  return (flags & kAreAnimationFlagNotLightSource) == 0;
}

std::size_t AreaAnimationsInfo::count_of(AreaAnimationKind kind) const noexcept {
  std::size_t count = 0;
  for (const auto& animation : animations) {
    if (animation.kind == kind) ++count;
  }
  return count;
}

bool parse_are_animations(const std::byte* data, std::size_t size,
                          AreaAnimationsInfo& out) noexcept {
  out = {};

  if (!data || size < sizeof(ARE_Header_st)) {
    return false;
  }

  ARE_Header_st header{};
  if (!read_struct(data, size, 0, header)) {
    return false;
  }
  if (header.nFileType != kAreFileType || header.nFileVersion != kAreFileVersion) {
    return false;
  }

  const auto count = static_cast<std::size_t>(header.nAnimations);
  if (count == 0) {
    return true;
  }
  if (count > kMaxAreaAnimationRecords) {
    return false;
  }

  const auto sectionOffset = static_cast<std::size_t>(header.nAnimationsOffset);
  const auto sectionBytes = count * sizeof(ARE_Animation_st);
  if (sectionOffset > size || sectionBytes > size - sectionOffset) {
    return false;
  }

  try {
    out.animations.reserve(count);
  } catch (...) {
    out = {};
    return false;
  }

  for (std::size_t i = 0; i < count; ++i) {
    ARE_Animation_st record{};
    if (!read_struct(data, size, sectionOffset + i * sizeof(ARE_Animation_st), record)) {
      out = {};
      return false;
    }

    try {
      out.animations.push_back(make_area_animation_info(record));
    } catch (...) {
      out = {};
      return false;
    }
  }

  return true;
}

namespace {
// Authored flame draw geometry from the game BAM frame tables. RenderBam
// draws the current frame's top-left at (CGameObject::m_pos - frameCenter),
// so relative to m_pos the flame's bottom-center sits at
// (w/2 - cx, h - cy) — dx/dy below. height/halfWidth are the authored
// footprint in world px.
struct FlameGeometry {
  std::string_view resref;
  float dx;
  float dy;
  float height;
  float halfWidth;
};
constexpr FlameGeometry kFlameGeometry[] = {
    {"FLAMBLU2", 4.0f, 15.0f, 15.0f, 4.0f}, {"FLMS", 0.0f, 3.0f, 5.0f, 1.5f},
    {"FLMSW", 0.0f, 3.0f, 5.0f, 1.5f},      {"FLSM1W", 0.0f, 10.0f, 20.0f, 10.0f},
    {"FLSM2W", 0.0f, 20.0f, 40.0f, 20.0f},  {"FLSM1RED", 9.0f, 19.0f, 20.0f, 10.0f},
    {"FLM1RED1", 0.0f, 4.0f, 20.0f, 10.0f}, {"FLML", 1.0f, 11.0f, 21.0f, 5.0f},
    {"FLMM", 1.0f, 8.0f, 15.0f, 4.0f},      {"YSFLBLU2", 0.0f, 0.0f, 15.0f, 4.0f},
    {"FIM1YLN1", 0.0f, 5.0f, 25.0f, 12.0f}, {"FIM2YLN2", 0.0f, 8.0f, 50.0f, 25.0f},
    {"FIRE", -6.0f, 30.0f, 48.0f, 6.0f},    {"FLAME2S", 0.0f, 3.0f, 12.0f, 6.0f},
    {"FLAME2L", 0.0f, 10.0f, 29.0f, 6.0f},  {"FPIT1S", 0.0f, 15.0f, 24.0f, 14.0f},
    {"FIRE_1", 1.0f, 25.0f, 50.0f, 17.0f},  {"FIRE_4", 0.0f, 15.0f, 27.0f, 7.0f},
};
constexpr FlameGeometry kDefaultFlameGeometry{"", 0.0f, 4.0f, 34.0f, 8.0f};

const FlameGeometry& flame_geometry_for(std::string_view resref) noexcept {
  for (const auto& entry : kFlameGeometry) {
    if (entry.resref == resref) return entry;
  }
  return kDefaultFlameGeometry;
}
}  // namespace

bool should_replace_animation_draw(std::string_view resref, AreaAnimationKind kind) noexcept {
  // Only standalone flame/smoke BAM families are replaced; per-area overlay
  // art (AM*/AR* hearth and plume images, name-classified unknowns) keeps
  // its engine draw.
  const auto upper = upper_copy(resref);
  if (kind == AreaAnimationKind::Fire) {
    return starts_with_any(upper, {"FLAM", "FIRE", "FPIT", "FLM", "FLSM", "YSFL", "FIM", "TORCH",
                                   "BRAZ"});
  }
  if (kind == AreaAnimationKind::Smoke) {
    return starts_with_any(upper, {"SMOK", "CHIM"});
  }
  return false;
}

std::vector<AreaEffectPoint> build_area_effect_points(const AreaAnimationsInfo& info) {
  std::vector<AreaEffectPoint> points;
  points.reserve((std::min)(info.animations.size(), kMaxAreaEffectPoints));

  const auto makePoint = [](const AreaAnimationInfo& animation)
      -> std::optional<AreaEffectPoint> {
    AreaEffectPoint point{};
    // The engine renders from the live object position, not the header
    // coordinates; both are logged so a divergence stays visible.
    point.x = static_cast<float>(animation.objX);
    point.y = static_cast<float>(animation.objY);
    const float base = static_cast<float>(static_cast<int>(animation.kind));
    const auto resref = animation.resrefView();
    switch (animation.kind) {
      case AreaAnimationKind::Fire: {
        point.kind = base;
        if (!should_replace_animation_draw(resref, animation.kind)) {
          // Overlay art keeps rendering; contribute warm cast light only.
          point.reserved1 = 2.0f;  // palette id: glow only
          point.height = 40.0f;
          point.halfWidth = 10.0f;
          return point;
        }
        // The ARE object position is already the flame's base point on screen
        // (ALIGN markers at raw objPos land exactly on the sconce bowls) — so
        // no positional offset is applied; the flame body grows upward from
        // the anchor. Only the authored SIZE varies per family. Live frame
        // geometry (when present) gives size directly; otherwise the table.
        // Note: while the effect suppresses the engine draw, CVidCell::m_pFrame
        // stays null, so the table is the normal path for replaced flames.
        const auto upper = upper_copy(resref);
        point.reserved1 = upper.find("BLU") != std::string::npos ? 1.0f : 0.0f;  // palette id
        if (animation.frameValid) {
          point.height = static_cast<float>(animation.frameHeight);
          point.halfWidth =
              (std::max)(static_cast<float>(animation.frameWidth) / 2.0f, 1.5f);
        } else {
          const auto& geometry = flame_geometry_for(upper);
          point.height = geometry.height;
          point.halfWidth = geometry.halfWidth;
        }
        return point;
      }
      case AreaAnimationKind::Smoke: {
        if (!should_replace_animation_draw(resref, animation.kind)) {
          return std::nullopt;  // authored plume art stays; nothing to add
        }
        point.kind = base;
        point.height = 170.0f;
        point.halfWidth = 11.0f;
        return point;
      }
      case AreaAnimationKind::Light: {
        point.kind = base;
        point.height = 54.0f;  // glow radius
        point.halfWidth = 0.0f;
        return point;
      }
      default:
        return std::nullopt;
    }
  };

  // Fire carries the effect's visual identity; when an area exceeds the
  // uniform capacity, drop smoke before light before fire.
  const AreaAnimationKind passes[] = {AreaAnimationKind::Fire, AreaAnimationKind::Light,
                                      AreaAnimationKind::Smoke};
  for (const auto pass : passes) {
    for (const auto& animation : info.animations) {
      if (animation.kind != pass || !animation.isShown()) continue;
      if (points.size() >= kMaxAreaEffectPoints) return points;
      if (const auto point = makePoint(animation)) points.push_back(*point);
    }
  }
  return points;
}

AreaAnimationInfo make_area_animation_info(const ARE_Animation_st& record) noexcept {
  AreaAnimationInfo animation{};
  animation.x = record.nX;
  animation.y = record.nY;
  animation.objX = record.nX;
  animation.objY = record.nY;
  animation.height = record.nHeight;
  animation.schedule = record.nSchedule;
  animation.flags = record.nFlags;
  animation.translucency = record.nTranslucency;
  animation.resref = copy_resref(record.rrAnimation);
  animation.name.fill('\0');
  for (std::size_t c = 0; c < record.szName.size(); ++c) {
    const auto character = static_cast<char>(record.szName[c]);
    if (character == '\0') break;
    animation.name[c] = character;
  }
  animation.kind = classify_area_animation(animation.resrefView(), animation.nameView());
  return animation;
}
}  // namespace iee::game
