#include "boltstream/broker/group_coordinator.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace boltstream::broker {

GroupCoordinatorError::GroupCoordinatorError(protocol::ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

GroupCoordinator::Result GroupCoordinator::join(std::string_view group, std::string_view topic,
                                                std::string_view member_id,
                                                std::uint32_t session_timeout_ms,
                                                std::uint16_t partition_count, TimePoint now) {
  std::lock_guard lock{mutex_};
  auto& state = groups_[GroupKey{std::string{group}, std::string{topic}}];
  if (state.group.empty()) {
    state.group = std::string{group};
    state.topic = std::string{topic};
  }
  state.partition_count = partition_count;

  Result result;
  result.expired_member_ids = expire_members(state, now);
  if (!result.expired_member_ids.empty()) {
    bump_generation(state);
    result.generation_changed = true;
  }
  auto expired_member_ids = std::move(result.expired_member_ids);
  const auto expired_generation_change = result.generation_changed;

  auto id = std::string{member_id};
  if (id.empty()) {
    id = next_member_id();
  }

  auto [it, inserted] = state.members.try_emplace(id);
  auto& member = it->second;
  if (inserted) {
    member.member_id = id;
    result.member_joined = true;
    bump_generation(state);
    result.generation_changed = true;
  }
  member.session_timeout_ms = session_timeout_ms;
  member.last_heartbeat = now;
  if (!inserted && member.assignment.empty()) {
    recompute_assignment(state);
  }

  result = make_result(state, &member);
  result.member_joined = inserted;
  result.generation_changed = expired_generation_change || inserted;
  result.expired_member_ids = std::move(expired_member_ids);
  return result;
}

GroupCoordinator::Result GroupCoordinator::sync(std::string_view group, std::string_view topic,
                                                std::string_view member_id,
                                                std::uint64_t generation_id, TimePoint now) {
  std::lock_guard lock{mutex_};
  const GroupKey key{std::string{group}, std::string{topic}};
  auto state_it = groups_.find(key);
  if (state_it == groups_.end()) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember, "consumer group is unknown"};
  }
  auto& state = state_it->second;

  Result result;
  result.expired_member_ids = expire_members(state, now);
  if (!result.expired_member_ids.empty()) {
    bump_generation(state);
    result.generation_changed = true;
  }
  auto expired_member_ids = std::move(result.expired_member_ids);
  const auto expired_generation_change = result.generation_changed;

  auto member_it = state.members.find(std::string{member_id});
  if (member_it == state.members.end()) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember, "group member is stale"};
  }
  if (generation_id != state.generation_id) {
    throw GroupCoordinatorError{protocol::ErrorCode::RebalanceRequired,
                                "group generation has changed"};
  }

  member_it->second.last_heartbeat = now;
  result = make_result(state, &member_it->second);
  result.generation_changed = expired_generation_change;
  result.expired_member_ids = std::move(expired_member_ids);
  return result;
}

GroupCoordinator::Result GroupCoordinator::heartbeat(std::string_view group, std::string_view topic,
                                                     std::string_view member_id,
                                                     std::uint64_t generation_id, TimePoint now) {
  std::lock_guard lock{mutex_};
  const GroupKey key{std::string{group}, std::string{topic}};
  auto state_it = groups_.find(key);
  if (state_it == groups_.end()) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember, "consumer group is unknown"};
  }
  auto& state = state_it->second;

  Result result;
  result.expired_member_ids = expire_members(state, now);
  if (!result.expired_member_ids.empty()) {
    bump_generation(state);
    result.generation_changed = true;
  }
  auto expired_member_ids = std::move(result.expired_member_ids);
  const auto expired_generation_change = result.generation_changed;

  auto member_it = state.members.find(std::string{member_id});
  if (member_it == state.members.end()) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember, "group member is stale"};
  }
  if (generation_id != state.generation_id) {
    throw GroupCoordinatorError{protocol::ErrorCode::RebalanceRequired,
                                "group generation has changed"};
  }

  member_it->second.last_heartbeat = now;
  result = make_result(state, &member_it->second);
  result.generation_changed = expired_generation_change;
  result.expired_member_ids = std::move(expired_member_ids);
  return result;
}

GroupCoordinator::Result GroupCoordinator::leave(std::string_view group, std::string_view topic,
                                                 std::string_view member_id,
                                                 std::uint64_t generation_id, TimePoint now) {
  std::lock_guard lock{mutex_};
  const GroupKey key{std::string{group}, std::string{topic}};
  auto state_it = groups_.find(key);
  if (state_it == groups_.end()) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember, "consumer group is unknown"};
  }
  auto& state = state_it->second;

  Result result;
  result.expired_member_ids = expire_members(state, now);
  if (!result.expired_member_ids.empty()) {
    bump_generation(state);
    result.generation_changed = true;
  }
  auto expired_member_ids = std::move(result.expired_member_ids);

  auto member_it = state.members.find(std::string{member_id});
  if (member_it == state.members.end()) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember, "group member is stale"};
  }
  if (generation_id != state.generation_id) {
    throw GroupCoordinatorError{protocol::ErrorCode::RebalanceRequired,
                                "group generation has changed"};
  }

  result = make_result(state, &member_it->second);
  state.members.erase(member_it);
  bump_generation(state);
  result.generation_id = state.generation_id;
  result.member_left = true;
  result.generation_changed = true;
  result.expired_member_ids = std::move(expired_member_ids);
  return result;
}

GroupCoordinator::Result GroupCoordinator::expire(std::string_view group, std::string_view topic,
                                                  TimePoint now) {
  std::lock_guard lock{mutex_};
  const GroupKey key{std::string{group}, std::string{topic}};
  auto state_it = groups_.find(key);
  if (state_it == groups_.end()) {
    Result result;
    result.group = std::string{group};
    result.topic = std::string{topic};
    return result;
  }
  auto& state = state_it->second;
  auto result = make_result(state);
  result.expired_member_ids = expire_members(state, now);
  if (!result.expired_member_ids.empty()) {
    bump_generation(state);
    result.generation_id = state.generation_id;
    result.generation_changed = true;
  }
  return result;
}

GroupCoordinator::Result GroupCoordinator::validate_commit(std::string_view group,
                                                           std::string_view topic,
                                                           std::string_view member_id,
                                                           std::uint64_t generation_id,
                                                           std::uint16_t partition, TimePoint now) {
  std::lock_guard lock{mutex_};
  const GroupKey key{std::string{group}, std::string{topic}};
  auto state_it = groups_.find(key);
  if (state_it == groups_.end()) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember, "consumer group is unknown"};
  }
  auto& state = state_it->second;

  Result result;
  result.expired_member_ids = expire_members(state, now);
  if (!result.expired_member_ids.empty()) {
    bump_generation(state);
    result.generation_changed = true;
  }
  auto expired_member_ids = std::move(result.expired_member_ids);
  const auto expired_generation_change = result.generation_changed;

  auto member_it = state.members.find(std::string{member_id});
  if (member_it == state.members.end()) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember, "group member is stale"};
  }
  if (generation_id != state.generation_id) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember,
                                "group generation is stale for commit"};
  }
  if (!owns_partition(member_it->second, partition)) {
    throw GroupCoordinatorError{protocol::ErrorCode::StaleMember,
                                "group member does not own committed partition"};
  }

  member_it->second.last_heartbeat = now;
  result = make_result(state, &member_it->second);
  result.generation_changed = expired_generation_change;
  result.expired_member_ids = std::move(expired_member_ids);
  return result;
}

bool GroupCoordinator::has_active_members(std::string_view group, std::string_view topic,
                                          TimePoint now) {
  std::lock_guard lock{mutex_};
  const GroupKey key{std::string{group}, std::string{topic}};
  auto state_it = groups_.find(key);
  if (state_it == groups_.end()) {
    return false;
  }
  auto& state = state_it->second;
  const auto expired = expire_members(state, now);
  if (!expired.empty()) {
    bump_generation(state);
  }
  return !state.members.empty();
}

std::size_t GroupCoordinator::active_member_count(std::string_view group, std::string_view topic,
                                                  TimePoint now) {
  std::lock_guard lock{mutex_};
  const GroupKey key{std::string{group}, std::string{topic}};
  auto state_it = groups_.find(key);
  if (state_it == groups_.end()) {
    return 0;
  }
  auto& state = state_it->second;
  const auto expired = expire_members(state, now);
  if (!expired.empty()) {
    bump_generation(state);
  }
  return state.members.size();
}

bool GroupCoordinator::topic_has_active_members(std::string_view topic, TimePoint now) {
  std::lock_guard lock{mutex_};
  bool active = false;
  for (auto& [key, state] : groups_) {
    if (key.topic != topic) {
      continue;
    }
    const auto expired = expire_members(state, now);
    if (!expired.empty()) {
      bump_generation(state);
    }
    if (!state.members.empty()) {
      active = true;
    }
  }
  return active;
}

void GroupCoordinator::remove_topic(std::string_view topic) {
  std::lock_guard lock{mutex_};
  for (auto it = groups_.begin(); it != groups_.end();) {
    if (it->first.topic == topic) {
      it = groups_.erase(it);
    } else {
      ++it;
    }
  }
}

GroupCoordinator::Result GroupCoordinator::make_result(const GroupState& state,
                                                       const MemberState* member) const {
  Result result;
  result.group = state.group;
  result.topic = state.topic;
  result.generation_id = state.generation_id;
  if (member != nullptr) {
    result.member_id = member->member_id;
    result.assignment = member->assignment;
  }
  return result;
}

std::vector<std::string> GroupCoordinator::expire_members(GroupState& state, TimePoint now) {
  std::vector<std::string> expired;
  for (auto it = state.members.begin(); it != state.members.end();) {
    const auto timeout = std::chrono::milliseconds(it->second.session_timeout_ms);
    if (now - it->second.last_heartbeat >= timeout) {
      expired.push_back(it->first);
      it = state.members.erase(it);
    } else {
      ++it;
    }
  }
  return expired;
}

void GroupCoordinator::bump_generation(GroupState& state) {
  ++state.generation_id;
  recompute_assignment(state);
}

void GroupCoordinator::recompute_assignment(GroupState& state) {
  for (auto& [_, member] : state.members) {
    member.assignment.clear();
  }
  if (state.members.empty() || state.partition_count == 0) {
    return;
  }

  const auto member_count = state.members.size();
  const auto base = state.partition_count / member_count;
  const auto remainder = state.partition_count % member_count;
  std::uint16_t next_partition = 0;
  std::size_t index = 0;
  for (auto& [_, member] : state.members) {
    const auto count = base + (index < remainder ? 1U : 0U);
    for (std::size_t assigned = 0; assigned < count; ++assigned) {
      member.assignment.push_back(next_partition++);
    }
    ++index;
  }
}

bool GroupCoordinator::owns_partition(const MemberState& member, std::uint16_t partition) {
  return std::find(member.assignment.begin(), member.assignment.end(), partition) !=
         member.assignment.end();
}

std::string GroupCoordinator::next_member_id() {
  std::ostringstream out;
  out << "member-" << std::setw(12) << std::setfill('0') << next_member_id_++;
  return out.str();
}

} // namespace boltstream::broker
