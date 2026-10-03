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

namespace {
// Authored flame/smoke cycles are short; anything beyond these bounds is
// treated as a stale or foreign pointer rather than walked.
constexpr std::size_t kMaxEnvelopeSequences = 64;
constexpr std::size_t kMaxEnvelopeFramesPerSequence = 256;
constexpr std::uint16_t kMaxEnvelopeFrameDimension = 1024;
constexpr std::uint16_t kBamNoFrame = 0xFFFF;

bool read_envelope_frame(const CResCell& cell, std::size_t frameIndex, std::size_t frameCount,
                         BamEnvelope& envelope) noexcept {
  if (frameIndex >= frameCount) return false;
  frameTableEntry_st frame{};
  if (!core::safe_read(cell.m_pFrames + frameIndex, frame)) return false;
  if (frame.nWidth > kMaxEnvelopeFrameDimension || frame.nHeight > kMaxEnvelopeFrameDimension) {
    return false;
  }
  extend_bam_envelope(envelope, frame);
  return true;
}
}  // namespace

bool read_static_bam_envelope(const void* staticObject, BamEnvelope& out) noexcept {
  out = {};
  if (!staticObject) return false;
  try {
    const auto* objectBytes = static_cast<const std::byte*>(staticObject);
    CVidCell vidCell{};
    ARE_Animation_st header{};
    if (!core::safe_read(objectBytes + offsetof(CGameStatic, m_vidCell), vidCell) ||
        !core::safe_read(objectBytes + offsetof(CGameStatic, m_header), header) ||
        !vidCell.pRes) {
      return false;
    }
    CResCell cell{};
    if (!core::safe_read(vidCell.pRes, cell) || !cell.baseclass_0.bLoaded ||
        !cell.baseclass_0.pData || !cell.m_pFrames || !cell.m_pSequences) {
      return false;
    }

    // V2 (PVRZ-backed) cycles index the frame table directly; V1 goes
    // through the frame lookup list.
    std::size_t frameCount = 0;
    std::size_t sequenceCount = 0;
    bool direct = false;
    if (cell.m_pBamHeaderV2) {
      BAMHEADERV2 v2{};
      if (!core::safe_read(cell.m_pBamHeaderV2, v2)) return false;
      frameCount = v2.nFrames;
      sequenceCount = v2.nSequences;
      direct = true;
    } else if (cell.m_pBamHeader && cell.m_pFrameList) {
      bamHeader_st v1{};
      if (!core::safe_read(cell.m_pBamHeader, v1)) return false;
      frameCount = v1.nFrames;
      sequenceCount = v1.nSequences;
    }
    if (frameCount == 0 || sequenceCount == 0) return false;

    std::size_t first = vidCell.m_nCurrentSequence < sequenceCount ? vidCell.m_nCurrentSequence : 0;
    std::size_t last = first + 1;
    if ((header.nFlags & kAreAnimationFlagAllSequences) != 0) {
      first = 0;
      last = (std::min)(sequenceCount, kMaxEnvelopeSequences);
    }

    BamEnvelope envelope{};
    for (std::size_t sequence = first; sequence < last; ++sequence) {
      sequenceTableEntry_st entry{};
      if (!core::safe_read(cell.m_pSequences + sequence, entry)) return false;
      if (entry.nFrames <= 0 || entry.nStartingFrame == kBamNoFrame) continue;
      const auto count =
          (std::min)(static_cast<std::size_t>(entry.nFrames), kMaxEnvelopeFramesPerSequence);
      for (std::size_t offset = 0; offset < count; ++offset) {
        std::size_t frameIndex = static_cast<std::size_t>(entry.nStartingFrame) + offset;
        if (!direct) {
          if (frameIndex >= cell.m_nFrameList) return false;
          std::uint16_t listed = kBamNoFrame;
          if (!core::safe_read(cell.m_pFrameList + frameIndex, listed)) return false;
          if (listed == kBamNoFrame) continue;
          frameIndex = listed;
        }
        if (!read_envelope_frame(cell, frameIndex, frameCount, envelope)) return false;
      }
    }
    if (!envelope.valid) return false;
    out = envelope;
    return true;
  } catch (...) {
    out = {};
    return false;
  }
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
      auto info = make_area_animation_info(record);
      // RenderBam draws from the live CGameObject position, not the header
      // coordinates; prefer it when readable.
      CPoint objectPos{};
      if (core::safe_read(objectBytes + offsetof(CGameObject, m_pos), objectPos)) {
        info.objX = objectPos.x;
        info.objY = objectPos.y;
      }
      std::int32_t objectPosZ = 0;
      if (core::safe_read(objectBytes + offsetof(CGameObject, m_posZ), objectPosZ)) {
        info.objZ = objectPosZ;
      }
      info.object = entry.m_objectPtr;
      // Valid only once the engine has loaded the BAM; the render-thread
      // envelope cache covers statics whose resource was released since.
      (void)read_static_bam_envelope(entry.m_objectPtr, info.envelope);
      out.animations.push_back(info);
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
