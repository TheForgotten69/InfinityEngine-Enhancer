#include "animation_interp.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"

namespace iee::game {
namespace {
constexpr std::uint32_t kBamSignature = 0x204D4142;   // "BAM "
constexpr std::uint32_t kBamVersion1 = 0x20203156;    // "V1  "
constexpr std::uint32_t kRawFrameFlag = 0x80000000u;
constexpr std::uint16_t kBamNoFrame = 0xFFFF;
constexpr std::size_t kPaletteBytes = 256 * 4;
constexpr std::size_t kMaxExpandedBamBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxSwapsPerScope = 32;

// True when [offset, offset + count * stride) lies inside `size`.
bool in_bounds(std::size_t offset, std::size_t count, std::size_t stride,
               std::size_t size) noexcept {
  return offset <= size && count <= (size - offset) / stride;
}
}  // namespace

bool parse_expanded_bam(std::span<std::byte> image, ExpandedBamView& out) noexcept {
  out = {};
  if (image.size() < sizeof(bamHeader_st)) return false;
  bamHeader_st header{};
  std::memcpy(&header, image.data(), sizeof(header));
  if (header.nFileType != kBamSignature || header.nFileVersion != kBamVersion1 ||
      header.nFrames == 0 || header.nSequences == 0) {
    return false;
  }
  const std::size_t size = image.size();
  const std::size_t framesOffset = header.nTableOffset;
  if (!in_bounds(framesOffset, header.nFrames, sizeof(frameTableEntry_st), size)) return false;
  const std::size_t sequencesOffset = framesOffset + header.nFrames * sizeof(frameTableEntry_st);
  if (!in_bounds(sequencesOffset, header.nSequences, sizeof(sequenceTableEntry_st), size) ||
      !in_bounds(header.nPaletteOffset, kPaletteBytes, 1, size) ||
      header.nFrameListOffset > size) {
    return false;
  }
  const std::size_t listEntries = (size - header.nFrameListOffset) / sizeof(std::uint16_t);

  for (std::size_t index = 0; index < header.nFrames; ++index) {
    frameTableEntry_st frame{};
    std::memcpy(&frame, image.data() + framesOffset + index * sizeof(frame), sizeof(frame));
    if ((frame.___u4 & kRawFrameFlag) == 0) return false;
    const std::size_t pixels = static_cast<std::size_t>(frame.nWidth) * frame.nHeight;
    if (!in_bounds(frame.___u4 & ~kRawFrameFlag, pixels, 1, size)) return false;
  }
  for (std::size_t index = 0; index < header.nSequences; ++index) {
    sequenceTableEntry_st sequence{};
    std::memcpy(&sequence, image.data() + sequencesOffset + index * sizeof(sequence),
                sizeof(sequence));
    if (sequence.nFrames < 0) return false;
    const auto count = static_cast<std::size_t>(sequence.nFrames);
    if (count == 0) continue;
    if (sequence.nStartingFrame > listEntries || count > listEntries - sequence.nStartingFrame) {
      return false;
    }
    for (std::size_t slot = 0; slot < count; ++slot) {
      std::uint16_t listed = 0;
      std::memcpy(&listed,
                  image.data() + header.nFrameListOffset +
                      (sequence.nStartingFrame + slot) * sizeof(listed),
                  sizeof(listed));
      // 0xFFFF is the format's "no frame in this slot"; the engine skips it.
      if (listed >= header.nFrames && listed != kBamNoFrame) return false;
    }
  }

  auto* base = image.data();
  out.header = reinterpret_cast<bamHeader_st*>(base);
  out.frames = reinterpret_cast<frameTableEntry_st*>(base + framesOffset);
  out.sequences = reinterpret_cast<sequenceTableEntry_st*>(base + sequencesOffset);
  out.frameList = reinterpret_cast<std::uint16_t*>(base + header.nFrameListOffset);
  out.palette = base + header.nPaletteOffset;
  return true;
}

std::uint16_t AnimationFrameTracker::sample(const void* key, std::uint16_t sequence,
                                            std::int16_t frame, std::int16_t length, double now) {
  auto [it, inserted] = states_.try_emplace(key);
  auto& state = it->second;
  const auto original = static_cast<std::uint16_t>(frame * 2);
  if (inserted || now - state.lastSeen > kForgetSeconds || sequence != state.sequence) {
    state = {sequence, frame, -1, now, kDefaultInterval, now};
    if (inserted && states_.size() > kPruneThreshold) {
      for (auto entry = states_.begin(); entry != states_.end();) {
        entry = now - entry->second.lastSeen > kForgetSeconds ? states_.erase(entry)
                                                              : std::next(entry);
      }
    }
    return original;
  }
  state.lastSeen = now;
  if (frame != state.frame) {
    const double observed = now - state.changedAt;
    state.interval =
        observed >= kMinInterval && observed <= kMaxInterval ? observed : kDefaultInterval;
    state.previous = state.frame;
    state.frame = frame;
    state.changedAt = now;
  }
  const bool consecutive =
      state.previous >= 0 && length > 1 && frame == (state.previous + 1) % length;
  if (consecutive && now - state.changedAt < state.interval / 2.0) {
    return static_cast<std::uint16_t>(state.previous * 2 + 1);
  }
  return original;
}

std::shared_ptr<AnimationInterpolator::Expanded> AnimationInterpolator::load(
    const std::string& resref) {
  if (const auto known = files_.find(resref); known != files_.end()) return known->second;
  std::shared_ptr<Expanded> result;
  std::error_code error;
  const auto path = directory_ / (resref + ".bam");
  const auto size = std::filesystem::file_size(path, error);
  if (!error && size >= sizeof(bamHeader_st) && size <= kMaxExpandedBamBytes) {
    auto expanded = std::make_shared<Expanded>();
    expanded->image.resize(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);
    if (file.read(reinterpret_cast<char*>(expanded->image.data()),
                  static_cast<std::streamsize>(size)) &&
        parse_expanded_bam(expanded->image, expanded->view)) {
      result = std::move(expanded);
      ++loaded_;
      LOG_INFO("Animation interpolation: loaded expanded BAM {} ({} frames)", resref,
               result->view.header->nFrames);
    } else {
      LOG_WARN("Animation interpolation: {} is not a valid expanded BAM; ignored",
               path.string());
    }
  } else if (!error) {
    LOG_WARN("Animation interpolation: {} has an unusable size; ignored", path.string());
  } else {
    LOG_INFO("Animation interpolation: no expanded BAM for {}", resref);
  }
  files_.emplace(resref, result);
  return result;
}

AnimationInterpolator::Resource* AnimationInterpolator::resource_for(CResCell* original) {
  if (const auto known = resources_.find(original); known != resources_.end()) {
    return known->second.get();
  }
  CResCell snapshot{};
  if (!core::safe_read(original, snapshot)) return nullptr;
  // The original must be a loaded BAM V1: its tables are the logic's view and
  // the template the private cell is copied from. Not cached until it is.
  if (!snapshot.baseclass_0.bLoaded || !snapshot.baseclass_0.pData || !snapshot.m_pBamHeader ||
      snapshot.m_pBamHeaderV2 || !snapshot.m_pSequences || !snapshot.baseclass_0.resref) {
    return nullptr;
  }
  // The name is an interned C string of at most 8 characters; read it one
  // byte at a time so nothing past its terminator is touched.
  std::array<char, 9> name{};
  for (std::size_t index = 0; index < 8; ++index) {
    if (!core::safe_read(snapshot.baseclass_0.resref + index, name[index]) || !name[index]) {
      name[index] = '\0';
      break;
    }
  }
  std::string resref(name.data());
  for (auto& character : resref) {
    character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  }

  auto resource = std::make_unique<Resource>();
  if (!resref.empty() && resref.find_first_of("/\\.:") == std::string::npos) {
    resource->expanded = load(resref);
  }
  if (resource->expanded) {
    const auto& view = resource->expanded->view;
    bamHeader_st originalHeader{};
    if (!core::safe_read(snapshot.m_pBamHeader, originalHeader) ||
        originalHeader.nSequences != view.header->nSequences) {
      LOG_WARN("Animation interpolation: expanded BAM {} does not match the game's cycle count; "
               "ignored",
               resref);
      resource->expanded.reset();
    } else {
      resource->cell = snapshot;
      resource->cell.baseclass_0.pData = resource->expanded->image.data();
      resource->cell.baseclass_0.nSize = static_cast<std::uint32_t>(resource->expanded->image.size());
      resource->cell.baseclass_0.bWasMalloced = false;
      resource->cell.baseclass_0.bLoaded = true;
      resource->cell.pUncompressedData = nullptr;
      resource->cell.nUncompressedSize = 0;
      resource->cell.m_pBamHeader = view.header;
      resource->cell.m_pBamHeaderV2 = nullptr;
      resource->cell.m_pQuads = nullptr;
      resource->cell.m_pFrames = view.frames;
      resource->cell.m_pSequences = view.sequences;
      resource->cell.m_pFrameList = view.frameList;
      resource->cell.m_pPalette = view.palette;
    }
  }
  return resources_.emplace(original, std::move(resource)).first->second.get();
}

void AnimationInterpolator::begin_scope(const void* owner) noexcept {
  if (depth_++ > 0) return;
  owner_ = owner;
  scopeTouched_ = 0;
  scopeMissed_ = false;
  observeOnly_ = true;
  try {
    if (const auto known = owners_.find(owner); known != owners_.end()) {
      observeOnly_ = !known->second;
    }
  } catch (...) {
  }
}

void AnimationInterpolator::touch(CVidCell* cell, double now) noexcept {
  if (depth_ <= 0 || !cell) return;
  try {
    for (const auto& swap : swaps_) {
      if (swap.cell == cell) return;
    }
    if (!cell->pRes) return;
    ++scopeTouched_;
    auto* original = cell->pRes;
    auto* resource = resource_for(original);
    if (!resource || !resource->expanded) {
      scopeMissed_ = true;
      return;
    }
    const auto sequence = cell->m_nCurrentSequence;
    const auto frame = cell->m_nCurrentFrame;
    const auto& view = resource->expanded->view;
    sequenceTableEntry_st logic{};
    if (sequence >= view.header->nSequences ||
        !core::safe_read(original->m_pSequences + sequence, logic) || logic.nFrames <= 0 ||
        frame < 0 || frame >= logic.nFrames ||
        view.sequences[sequence].nFrames != logic.nFrames * 2) {
      scopeMissed_ = true;
      return;
    }
    if (observeOnly_ || swaps_.size() >= kMaxSwapsPerScope) return;
    const auto shown = tracker_.sample(cell, sequence, frame, logic.nFrames, now);
    swaps_.push_back({cell, original, frame});
    cell->pRes = &resource->cell;
    cell->m_nCurrentFrame = static_cast<std::int16_t>(shown);
    cell->m_pFrame = nullptr;
  } catch (...) {
    // Interpolation is cosmetic; any doubt draws the original frame.
    scopeMissed_ = true;
  }
}

void AnimationInterpolator::end_scope() noexcept {
  if (depth_ <= 0) return;
  if (--depth_ > 0) return;
  for (const auto& swap : swaps_) {
    // Restore only our own swap: if the engine re-pointed the cell during the
    // draw, its resource is the newer one and must win.
    bool ours = false;
    for (const auto& [original, resource] : resources_) {
      if (swap.cell->pRes == &resource->cell) {
        ours = true;
        break;
      }
    }
    if (!ours) continue;
    swap.cell->pRes = swap.original;
    swap.cell->m_nCurrentFrame = swap.frame;
    swap.cell->m_pFrame = nullptr;
  }
  swaps_.clear();
  try {
    owners_[owner_] = scopeTouched_ > 0 && !scopeMissed_;
  } catch (...) {
  }
}
}  // namespace iee::game
