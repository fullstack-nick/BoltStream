#pragma once

#include <cstdint>
#include <span>

namespace boltstream::benchmarking {

double nearest_rank_microseconds(std::span<const std::uint64_t> sorted_nanoseconds,
                                 double quantile);
double median(std::span<const double> values);
double coefficient_of_variation(std::span<const double> values);

} // namespace boltstream::benchmarking
