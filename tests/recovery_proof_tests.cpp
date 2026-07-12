#include "boltstream/recovery/proof.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(std::string_view suffix) {
    path = std::filesystem::temp_directory_path() /
           ("boltstream-recovery-proof-" + std::string{suffix} + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void prove(boltstream::recovery::FaultScenario scenario) {
  TempDir temp{boltstream::recovery::scenario_name(scenario)};
  boltstream::recovery::stage_crash_state(temp.path, scenario);
  const auto result = boltstream::recovery::verify_crash_recovery(temp.path, scenario);
  EXPECT_EQ(result.records_recovered, 3U);
  EXPECT_EQ(result.next_offset, 3U);
  EXPECT_EQ(result.indexes_rebuilt, 1U);
  EXPECT_EQ(result.index_bytes_after, 60U);
}

} // namespace

TEST(RecoveryProofTests, TornRecordKeepsExactlyCommittedPrefix) {
  prove(boltstream::recovery::FaultScenario::TornRecord);
}

TEST(RecoveryProofTests, PartialZstdBatchKeepsExactlyCommittedPrefix) {
  prove(boltstream::recovery::FaultScenario::PartialBatch);
}

TEST(RecoveryProofTests, PartialIndexIsRebuiltFromCanonicalLog) {
  prove(boltstream::recovery::FaultScenario::StaleIndex);
}
