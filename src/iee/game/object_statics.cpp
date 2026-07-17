#include "object_statics.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

#include "iee/core/pattern_scanner.h"

namespace iee::game {
namespace {
// GetShare instruction encodings holding the RIP-relative globals:
//   66 39 05 <disp32>   cmp WORD PTR [rip+disp], ax   -> m_maxArrayIndex
//   4C 8D 05 <disp32>   lea r8, [rip+disp]            -> entry table
constexpr std::size_t kRipInstructionSize = 7;

const std::byte* decode_rip_operand(const std::byte* instruction) noexcept {
  std::int32_t displacement = 0;
  std::memcpy(&displacement, instruction + 3, sizeof(displacement));
  return instruction + kRipInstructionSize + displacement;
}

bool matches(const std::byte* code, std::initializer_list<std::uint8_t> bytes) noexcept {
  std::size_t index = 0;
  for (const auto expected : bytes) {
    if (std::to_integer<std::uint8_t>(code[index]) != expected) return false;
    ++index;
  }
  return true;
}
}  // namespace

bool decode_object_array_globals(const std::byte* function, std::size_t windowSize,
                                 ObjectArrayGlobals& out) noexcept {
  out = {};
  if (!function || windowSize < kRipInstructionSize ||
      !core::is_readable(function, windowSize)) {
    return false;
  }

  const std::byte* maxIndexAddress = nullptr;
  const std::byte* entriesAddress = nullptr;
  bool ambiguous = false;
  for (std::size_t offset = 0; offset + kRipInstructionSize <= windowSize; ++offset) {
    const auto* code = function + offset;
    if (matches(code, {0x66, 0x39, 0x05})) {
      if (maxIndexAddress) ambiguous = true;
      maxIndexAddress = decode_rip_operand(code);
    } else if (matches(code, {0x4C, 0x8D, 0x05})) {
      if (entriesAddress) ambiguous = true;
      entriesAddress = decode_rip_operand(code);
    }
  }

  if (ambiguous || !maxIndexAddress || !entriesAddress) {
    return false;
  }

  out.maxArrayIndex = reinterpret_cast<const std::int16_t*>(maxIndexAddress);
  out.entries = reinterpret_cast<const CGameObjectArrayEntry*>(entriesAddress);
  return true;
}

bool collect_area_static_animations(const ObjectArrayGlobals& globals, const CGameArea* area,
                                    AreaAnimationsInfo& out) noexcept {
  out = {};
  if (!globals.valid() || !area) {
    return false;
  }

  std::int16_t maxIndex = 0;
  if (!core::safe_read(globals.maxArrayIndex, maxIndex) || maxIndex < 0) {
    return false;
  }
  const auto entryCount =
      (std::min)(static_cast<std::size_t>(maxIndex) + 1, kObjectArrayMaxEntries);

  try {
    for (std::size_t index = 0; index < entryCount; ++index) {
      CGameObjectArrayEntry entry{};
      if (!core::safe_read(globals.entries + index, entry) || !entry.m_objectPtr) {
        continue;
      }

      const auto* objectBytes = reinterpret_cast<const std::byte*>(entry.m_objectPtr);
      std::uint8_t objectType = 0;
      if (!core::safe_read(objectBytes + offsetof(CGameObject, m_objectType), objectType) ||
          objectType != kGameObjectTypeStatic) {
        continue;
      }
      const CGameArea* owner = nullptr;
      if (!core::safe_read(objectBytes + offsetof(CGameObject, m_pArea), owner) ||
          owner != area) {
        continue;
      }

      ARE_Animation_st record{};
      if (!core::safe_read(objectBytes + offsetof(CGameStatic, m_header), record)) {
        continue;
      }
      out.animations.push_back(make_area_animation_info(record));
      if (out.animations.size() >= kMaxAreaAnimationRecords) {
        break;
      }
    }
  } catch (...) {
    out = {};
    return false;
  }

  return true;
}
}  // namespace iee::game
