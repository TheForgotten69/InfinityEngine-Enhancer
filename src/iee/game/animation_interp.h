#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime_types_x64.h"

namespace iee::game {
// Table pointers into a validated expanded BAM V1 image, the same four that
// CResCell::Parse derives.
struct ExpandedBamView {
  bamHeader_st* header{};
  frameTableEntry_st* frames{};
  sequenceTableEntry_st* sequences{};
  std::uint16_t* frameList{};
  void* palette{};
};

// Validates an uncompressed BAM V1 image end to end (tables, lookup entries
// and every frame's raw pixel block inside the image) before the engine is
// ever pointed at it. Only raw frames are accepted: an RLE frame's length
// cannot be bounded without decoding it.
[[nodiscard]] bool parse_expanded_bam(std::span<std::byte> image, ExpandedBamView& out) noexcept;

// Chooses which expanded frame to draw for a cell whose logic frame only
// changes on the engine's animation tick. An expanded cycle holds original
// frame k at 2k and the in-between leading to k+1 at 2k+1; for the first half
// of the observed tick interval after a step k-1 -> k the in-between is shown,
// then frame k. The picture therefore trails the logic by half a step and
// never guesses a frame the engine has not reached.
class AnimationFrameTracker {
 public:
  static constexpr double kDefaultInterval = 1.0 / 15.0;
  static constexpr double kMinInterval = 1.0 / 90.0;
  static constexpr double kMaxInterval = 1.0 / 4.0;
  static constexpr double kForgetSeconds = 0.5;
  static constexpr std::size_t kPruneThreshold = 4096;

  // `frame` is the logic frame within a cycle of `length` original frames.
  [[nodiscard]] std::uint16_t sample(const void* key, std::uint16_t sequence, std::int16_t frame,
                                     std::int16_t length, double now);
  void clear() noexcept { states_.clear(); }

 private:
  struct State {
    std::uint16_t sequence{};
    std::int16_t frame{};
    std::int16_t previous{-1};
    double changedAt{};
    double interval{kDefaultInterval};
    double lastSeen{};
  };
  std::unordered_map<const void*, State> states_;
};

// Draw-time substitution of expanded (frame-interpolated) BAMs.
//
// Game logic keeps the original BAM and frame number. While a creature is
// being drawn (a scope), each of its CVidCells is pointed at a private
// CResCell over the expanded image and at the in-between frame for the
// current instant; closing the scope puts the original resource and logic
// frame back. Expanded files are read from `directory/<RESREF>.bam` on first
// use; a BAM without one draws exactly as before.
//
// A creature is drawn from several frame-synced cells (body, weapon, shield,
// helmet), each its own BAM. Swapping only some of them would draw the body
// half a step behind its weapon, so a creature is interpolated only when
// every cell it drew in its previous scope had an expanded BAM: each owner's
// first scope (and any scope after a miss) only observes.
//
// Single-threaded: every call must come from the engine's render thread.
class AnimationInterpolator {
 public:
  void set_directory(std::filesystem::path directory) { directory_ = std::move(directory); }

  // `owner` identifies the creature being drawn.
  void begin_scope(const void* owner) noexcept;
  // Restores every cell swapped since the matching begin_scope and records
  // whether the owner's cells were all covered.
  void end_scope() noexcept;
  // Inside a scope: swap `cell` if it is not swapped yet and an expanded BAM
  // exists for its resource. Outside a scope, or on any doubt, does nothing.
  void touch(CVidCell* cell, double now) noexcept;
  // Area change: forget per-cell animation timing (loaded files are kept).
  void reset_timing() noexcept {
    tracker_.clear();
    owners_.clear();
  }

  [[nodiscard]] std::size_t loaded_count() const noexcept { return loaded_; }

 private:
  struct Expanded {
    std::vector<std::byte> image;
    ExpandedBamView view;
  };
  struct Resource {
    std::shared_ptr<Expanded> expanded;  // null: no usable expanded BAM
    CResCell cell{};                     // private copy pointing into `expanded`
  };
  struct Swap {
    CVidCell* cell{};
    CResCell* original{};
    std::int16_t frame{};
  };

  Resource* resource_for(CResCell* original);
  std::shared_ptr<Expanded> load(const std::string& resref);

  std::filesystem::path directory_;
  int depth_{};
  const void* owner_{};
  bool observeOnly_{true};
  bool scopeMissed_{};
  std::size_t scopeTouched_{};
  std::unordered_map<const void*, bool> owners_;  // owner -> last scope fully covered
  std::vector<Swap> swaps_;
  std::unordered_map<const CResCell*, std::unique_ptr<Resource>> resources_;
  std::unordered_map<std::string, std::shared_ptr<Expanded>> files_;
  AnimationFrameTracker tracker_;
  std::size_t loaded_{};
};
}  // namespace iee::game
