#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace iee::core {

struct PerformanceSampleSummary {
  std::size_t count{};
  std::size_t dropped{};
  double average{};
  double percentile95{};
  double maximum{};
};

template <std::size_t Capacity>
class PerformanceSamples {
 public:
  static_assert(Capacity > 0, "PerformanceSamples requires storage");

  void add(double value) noexcept {
    if (size_ < values_.size()) {
      values_[size_++] = value;
    } else {
      ++dropped_;
    }
  }

  [[nodiscard]] PerformanceSampleSummary summarize() const noexcept {
    PerformanceSampleSummary summary{.count = size_, .dropped = dropped_};
    if (size_ == 0) return summary;

    auto ordered = values_;
    const auto percentileIndex = (size_ * 95 + 99) / 100 - 1;
    std::nth_element(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(percentileIndex),
                     ordered.begin() + static_cast<std::ptrdiff_t>(size_));
    summary.percentile95 = ordered[percentileIndex];

    double total = 0.0;
    double maximum = values_[0];
    for (std::size_t index = 0; index < size_; ++index) {
      total += values_[index];
      maximum = (std::max)(maximum, values_[index]);
    }
    summary.average = total / static_cast<double>(size_);
    summary.maximum = maximum;
    return summary;
  }

  void reset() noexcept {
    size_ = 0;
    dropped_ = 0;
  }

 private:
  std::array<double, Capacity> values_{};
  std::size_t size_{};
  std::size_t dropped_{};
};

}  // namespace iee::core
