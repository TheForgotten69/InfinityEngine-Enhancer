#pragma once

#include <cstddef>
#include <cstdint>

#include "are_animations.h"
#include "runtime_types_x64.h"

namespace iee::game {
// Resolved CGameObjectArray statics. On the validated builds the entry table
// is a fixed static array (not a heap pointer): CGameObjectArray::GetShare
// loads its address with a RIP-relative lea. See
// docs/are-animation-detection.md for the reverse-engineering evidence.
struct ObjectArrayGlobals {
  const CGameObjectArrayEntry* entries{};
  const std::int16_t* maxArrayIndex{};

  [[nodiscard]] bool valid() const noexcept { return entries != nullptr && maxArrayIndex != nullptr; }
};

// 15-bit index space per the GetShare locator encoding.
inline constexpr std::size_t kObjectArrayMaxEntries = 0x8000;

// Decodes the object-array globals out of CGameObjectArray::GetShare's body:
// `cmp WORD PTR [rip+d], ax` (m_maxArrayIndex) and `lea r8, [rip+d]` (entry
// table). `function` points at the manifest pattern match; the scan is
// bounded to `windowSize` bytes and each instruction must occur exactly once
// or the decode fails closed.
[[nodiscard]] bool decode_object_array_globals(const std::byte* function, std::size_t windowSize,
                                               ObjectArrayGlobals& out) noexcept;

// Walks the engine object array and collects the authored static-animation
// records (CGameStatic::m_header) owned by `area` into classified entries.
// Returns false when the array is unresolved or unreadable; an area with no
// statics yields true with an empty list.
[[nodiscard]] bool collect_area_static_animations(const ObjectArrayGlobals& globals,
                                                  const CGameArea* area,
                                                  AreaAnimationsInfo& out) noexcept;
}  // namespace iee::game
