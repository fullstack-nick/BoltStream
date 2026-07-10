#include "boltstream/benchmark/statistics.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace boltstream::benchmarking {

double nearest_rank_microseconds(std::span<const std::uint64_t> sorted_nanoseconds,
                                 double quantile) {
  if (sorted_nanoseconds.empty()) {
    return 0.0;
  }
  const auto bounded_quantile = std::clamp(quantile, 0.0, 1.0);
  const auto rank = static_cast<std::size_t>(
      std::ceil(bounded_quantile * static_cast<double>(sorted_nanoseconds.size())));
  return static_cast<double>(sorted_nanoseconds[std::max<std::size_t>(1, rank) - 1]) / 1000.0;
}

double median(std::span<const double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::vector<double> sorted{values.begin(), values.end()};
  std::sort(sorted.begin(), sorted.end());
  const auto middle = sorted.size() / 2;
  return sorted.size() % 2 == 0 ? (sorted[middle - 1] + sorted[middle]) / 2.0 : sorted[middle];
}

double coefficient_of_variation(std::span<const double> values) {
  if (values.size() < 2) {
    return 0.0;
  }
  const auto mean =
      std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  if (mean == 0.0) {
    return 0.0;
  }
  double squares = 0.0;
  for (const auto value : values) {
    squares += (value - mean) * (value - mean);
  }
  return std::sqrt(squares / static_cast<double>(values.size() - 1)) / mean * 100.0;
}

} // namespace boltstream::benchmarking
