#pragma once

#include "boltstream/protocol/protocol.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace boltstream::broker {

class GroupCoordinatorError : public std::runtime_error {
public:
  GroupCoordinatorError(protocol::ErrorCode code, std::string message);

  [[nodiscard]] protocol::ErrorCode code() const { return code_; }

private:
  protocol::ErrorCode code_;
};

class GroupCoordinator {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  struct Result {
    std::string group;
    std::string topic;
    std::string member_id;
    std::uint64_t generation_id{0};
    std::vector<std::uint16_t> assignment;
    std::vector<std::string> expired_member_ids;
    bool member_joined{false};
    bool member_left{false};
    bool generation_changed{false};
  };

  Result join(std::string_view group, std::string_view topic, std::string_view member_id,
              std::uint32_t session_timeout_ms, std::uint16_t partition_count,
              TimePoint now = Clock::now());
  Result sync(std::string_view group, std::string_view topic, std::string_view member_id,
              std::uint64_t generation_id, TimePoint now = Clock::now());
  Result heartbeat(std::string_view group, std::string_view topic, std::string_view member_id,
                   std::uint64_t generation_id, TimePoint now = Clock::now());
  Result leave(std::string_view group, std::string_view topic, std::string_view member_id,
               std::uint64_t generation_id, TimePoint now = Clock::now());
  Result expire(std::string_view group, std::string_view topic, TimePoint now = Clock::now());
  Result validate_commit(std::string_view group, std::string_view topic, std::string_view member_id,
                         std::uint64_t generation_id, std::uint16_t partition,
                         TimePoint now = Clock::now());
  bool has_active_members(std::string_view group, std::string_view topic,
                          TimePoint now = Clock::now());
  std::size_t active_member_count(std::string_view group, std::string_view topic,
                                  TimePoint now = Clock::now());
  bool topic_has_active_members(std::string_view topic, TimePoint now = Clock::now());
  void remove_topic(std::string_view topic);

private:
  struct GroupKey {
    std::string group;
    std::string topic;

    bool operator<(const GroupKey& other) const {
      if (group != other.group) {
        return group < other.group;
      }
      return topic < other.topic;
    }
  };

  struct MemberState {
    std::string member_id;
    std::uint32_t session_timeout_ms{0};
    TimePoint last_heartbeat{};
    std::vector<std::uint16_t> assignment;
  };

  struct GroupState {
    std::string group;
    std::string topic;
    std::uint16_t partition_count{0};
    std::uint64_t generation_id{0};
    std::map<std::string, MemberState> members;
  };

  Result make_result(const GroupState& state, const MemberState* member = nullptr) const;
  std::vector<std::string> expire_members(GroupState& state, TimePoint now);
  void bump_generation(GroupState& state);
  static void recompute_assignment(GroupState& state);
  static bool owns_partition(const MemberState& member, std::uint16_t partition);
  [[nodiscard]] std::string next_member_id();

  std::mutex mutex_;
  std::map<GroupKey, GroupState> groups_;
  std::uint64_t next_member_id_{1};
};

} // namespace boltstream::broker
