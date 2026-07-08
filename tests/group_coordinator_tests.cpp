#include "boltstream/broker/group_coordinator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;

TEST(GroupCoordinatorTests, AssignsPartitionsDeterministicallyBySortedMemberId) {
  boltstream::broker::GroupCoordinator coordinator;
  const auto start = boltstream::broker::GroupCoordinator::Clock::now();

  const auto first = coordinator.join("dashboard", "trades", "", 1000, 4, start);
  ASSERT_EQ(first.member_id, "member-000000000001");
  EXPECT_EQ(first.generation_id, 1U);
  EXPECT_EQ(first.assignment, (std::vector<std::uint16_t>{0, 1, 2, 3}));

  const auto second = coordinator.join("dashboard", "trades", "", 1000, 4, start + 10ms);
  ASSERT_EQ(second.member_id, "member-000000000002");
  EXPECT_EQ(second.generation_id, 2U);
  EXPECT_EQ(second.assignment, (std::vector<std::uint16_t>{2, 3}));

  const auto synced_first =
      coordinator.sync("dashboard", "trades", first.member_id, 2, start + 20ms);
  EXPECT_EQ(synced_first.assignment, (std::vector<std::uint16_t>{0, 1}));
}

TEST(GroupCoordinatorTests, RejoiningKnownMemberKeepsGenerationStable) {
  boltstream::broker::GroupCoordinator coordinator;
  const auto start = boltstream::broker::GroupCoordinator::Clock::now();

  const auto joined = coordinator.join("dashboard", "trades", "", 1000, 3, start);
  const auto rejoined =
      coordinator.join("dashboard", "trades", joined.member_id, 1000, 3, start + 100ms);

  EXPECT_EQ(rejoined.member_id, joined.member_id);
  EXPECT_EQ(rejoined.generation_id, joined.generation_id);
  EXPECT_FALSE(rejoined.generation_changed);
  EXPECT_EQ(rejoined.assignment, (std::vector<std::uint16_t>{0, 1, 2}));
}

TEST(GroupCoordinatorTests, LeaveTriggersRebalance) {
  boltstream::broker::GroupCoordinator coordinator;
  const auto start = boltstream::broker::GroupCoordinator::Clock::now();

  const auto first = coordinator.join("dashboard", "trades", "", 1000, 4, start);
  const auto second = coordinator.join("dashboard", "trades", "", 1000, 4, start + 10ms);

  const auto left = coordinator.leave("dashboard", "trades", second.member_id, second.generation_id,
                                      start + 20ms);
  EXPECT_TRUE(left.member_left);
  EXPECT_EQ(left.generation_id, 3U);

  const auto synced_first =
      coordinator.sync("dashboard", "trades", first.member_id, 3, start + 30ms);
  EXPECT_EQ(synced_first.assignment, (std::vector<std::uint16_t>{0, 1, 2, 3}));
}

TEST(GroupCoordinatorTests, TimeoutExpiresMemberAndRequiresRebalance) {
  boltstream::broker::GroupCoordinator coordinator;
  const auto start = boltstream::broker::GroupCoordinator::Clock::now();

  const auto first = coordinator.join("dashboard", "trades", "", 100, 4, start);
  (void)coordinator.join("dashboard", "trades", "", 100, 4, start + 10ms);
  (void)coordinator.heartbeat("dashboard", "trades", first.member_id, 2, start + 50ms);

  const auto expired = coordinator.expire("dashboard", "trades", start + 120ms);
  EXPECT_TRUE(expired.generation_changed);
  EXPECT_EQ(expired.expired_member_ids, (std::vector<std::string>{"member-000000000002"}));
  EXPECT_EQ(expired.generation_id, 3U);

  const auto synced_first =
      coordinator.sync("dashboard", "trades", first.member_id, 3, start + 130ms);
  EXPECT_EQ(synced_first.assignment, (std::vector<std::uint16_t>{0, 1, 2, 3}));
}

TEST(GroupCoordinatorTests, StaleCommitIsRejected) {
  boltstream::broker::GroupCoordinator coordinator;
  const auto start = boltstream::broker::GroupCoordinator::Clock::now();

  const auto first = coordinator.join("dashboard", "trades", "", 1000, 2, start);
  (void)coordinator.join("dashboard", "trades", "", 1000, 2, start + 10ms);

  try {
    (void)coordinator.validate_commit("dashboard", "trades", first.member_id, first.generation_id,
                                      0, start + 20ms);
    FAIL() << "expected stale generation commit to fail";
  } catch (const boltstream::broker::GroupCoordinatorError& error) {
    EXPECT_EQ(error.code(), boltstream::protocol::ErrorCode::StaleMember);
  }
}
