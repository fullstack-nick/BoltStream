#include "boltstream/benchmark/statistics.h"

#include <gtest/gtest.h>

#include <array>

TEST(BenchmarkStatisticsTests, NearestRankHandlesEmptyAndBoundarySamples) {
  constexpr std::array<std::uint64_t, 0> empty{};
  constexpr std::array<std::uint64_t, 4> samples{1000, 2000, 3000, 4000};

  EXPECT_DOUBLE_EQ(boltstream::benchmarking::nearest_rank_microseconds(empty, 0.99), 0.0);
  EXPECT_DOUBLE_EQ(boltstream::benchmarking::nearest_rank_microseconds(samples, 0.0), 1.0);
  EXPECT_DOUBLE_EQ(boltstream::benchmarking::nearest_rank_microseconds(samples, 0.50), 2.0);
  EXPECT_DOUBLE_EQ(boltstream::benchmarking::nearest_rank_microseconds(samples, 0.95), 4.0);
  EXPECT_DOUBLE_EQ(boltstream::benchmarking::nearest_rank_microseconds(samples, 1.0), 4.0);
}

TEST(BenchmarkStatisticsTests, MedianAndCoefficientOfVariationUseSampleDispersion) {
  constexpr std::array<double, 3> odd{1.0, 9.0, 5.0};
  constexpr std::array<double, 4> even{4.0, 1.0, 3.0, 2.0};
  constexpr std::array<double, 3> stable{10.0, 10.0, 10.0};
  constexpr std::array<double, 2> dispersed{10.0, 20.0};

  EXPECT_DOUBLE_EQ(boltstream::benchmarking::median(odd), 5.0);
  EXPECT_DOUBLE_EQ(boltstream::benchmarking::median(even), 2.5);
  EXPECT_DOUBLE_EQ(boltstream::benchmarking::coefficient_of_variation(stable), 0.0);
  EXPECT_NEAR(boltstream::benchmarking::coefficient_of_variation(dispersed), 47.140452, 0.000001);
}
