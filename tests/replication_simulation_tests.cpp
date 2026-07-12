#include "boltstream/replication/simulation.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct TemporaryCluster {
  std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("boltstream-replication-test-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~TemporaryCluster() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

std::vector<std::uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }

boltstream::replication::SimulationOptions options(const TemporaryCluster& cluster) {
  boltstream::replication::SimulationOptions result;
  result.leader_data_dir = cluster.root / "leader";
  result.follower_data_dir = cluster.root / "follower";
  result.topic = "events";
  return result;
}

TEST(ReplicationSimulationTests, FollowerCatchesUpAndPublishesBoundedLagMetrics) {
  TemporaryCluster cluster;
  boltstream::replication::ReplicationSimulation simulation{options(cluster)};
  const auto key = bytes("key");
  const auto value = bytes("compressible-compressible-compressible");
  const std::vector<boltstream::storage::AppendRecord> records{{key, value}, {key, value}};

  const auto produced = simulation.produce(records, boltstream::compression::Codec::Zstd,
                                           boltstream::replication::AcknowledgementMode::Leader);
  EXPECT_EQ(produced.next_offset, 2U);
  const auto lagging = simulation.status();
  EXPECT_EQ(lagging.assignment.leader_id, "leader-1");
  ASSERT_EQ(lagging.assignment.follower_ids.size(), 1U);
  EXPECT_EQ(lagging.assignment.follower_ids.front(), "follower-1");
  EXPECT_EQ(lagging.lag_records, 2U);
  EXPECT_EQ(simulation.catch_up(), 1U);
  EXPECT_FALSE(simulation.replicate_once());
  EXPECT_EQ(simulation.status().lag_records, 0U);

  const auto leader = simulation.leader_records();
  const auto follower = simulation.follower_records();
  ASSERT_EQ(follower.size(), leader.size());
  for (std::size_t index = 0; index < leader.size(); ++index) {
    EXPECT_EQ(follower[index].metadata.offset, leader[index].metadata.offset);
    EXPECT_EQ(follower[index].key, leader[index].key);
    EXPECT_EQ(follower[index].value, leader[index].value);
  }
  const auto metrics = simulation.prometheus_metrics();
  EXPECT_NE(metrics.find("boltstream_replication_lag_records"), std::string::npos);
  EXPECT_NE(metrics.find("topic=\"events\",partition=\"0\""), std::string::npos);
}

TEST(ReplicationSimulationTests, RejectsFollowerOffsetAheadOfLeader) {
  TemporaryCluster cluster;
  const auto configured = options(cluster);
  auto follower = boltstream::storage::PartitionLog::open(
      {configured.follower_data_dir, configured.topic, configured.partition});
  const auto key = bytes("key");
  const auto value = bytes("value");
  follower.append(key, value);

  EXPECT_THROW(boltstream::replication::ReplicationSimulation{configured}, std::runtime_error);
}

TEST(ReplicationSimulationTests, AcknowledgementModesRespectFollowerAvailability) {
  TemporaryCluster cluster;
  boltstream::replication::ReplicationSimulation simulation{options(cluster)};
  const auto key = bytes("key");
  const auto value = bytes("value");
  const boltstream::storage::AppendRecord record{key, value};
  simulation.set_follower_available(false);

  EXPECT_NO_THROW(simulation.produce(std::span<const boltstream::storage::AppendRecord>{&record, 1},
                                     boltstream::compression::Codec::None,
                                     boltstream::replication::AcknowledgementMode::Leader));
  EXPECT_THROW(simulation.produce(std::span<const boltstream::storage::AppendRecord>{&record, 1},
                                  boltstream::compression::Codec::None,
                                  boltstream::replication::AcknowledgementMode::All,
                                  std::chrono::milliseconds{5}),
               boltstream::replication::ReplicationTimeout);
  EXPECT_EQ(simulation.status().leader_next_offset, 2U);
  EXPECT_EQ(simulation.status().follower_next_offset, 0U);

  simulation.set_follower_available(true);
  EXPECT_EQ(simulation.catch_up(), 2U);
  EXPECT_EQ(simulation.status().lag_records, 0U);
  EXPECT_NO_THROW(simulation.produce(std::span<const boltstream::storage::AppendRecord>{&record, 1},
                                     boltstream::compression::Codec::None,
                                     boltstream::replication::AcknowledgementMode::All));
  EXPECT_EQ(simulation.status().lag_records, 0U);
}

TEST(ReplicationSimulationTests, ReopenedFollowerResumesAtRecoveredOffset) {
  TemporaryCluster cluster;
  boltstream::replication::ReplicationSimulation simulation{options(cluster)};
  const auto key = bytes("key");
  const auto first = bytes("first");
  const boltstream::storage::AppendRecord first_record{key, first};
  simulation.produce(std::span<const boltstream::storage::AppendRecord>{&first_record, 1},
                     boltstream::compression::Codec::None,
                     boltstream::replication::AcknowledgementMode::All);
  ASSERT_EQ(simulation.status().follower_next_offset, 1U);

  simulation.reopen_follower();
  EXPECT_EQ(simulation.status().follower_next_offset, 1U);
  const auto second = bytes("second");
  const boltstream::storage::AppendRecord second_record{key, second};
  simulation.produce(std::span<const boltstream::storage::AppendRecord>{&second_record, 1},
                     boltstream::compression::Codec::None,
                     boltstream::replication::AcknowledgementMode::Leader);
  EXPECT_EQ(simulation.status().lag_records, 1U);
  EXPECT_EQ(simulation.catch_up(), 1U);
  ASSERT_EQ(simulation.follower_records().size(), 2U);
}

} // namespace
