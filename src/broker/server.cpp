#include "boltstream/broker/server.h"

#include "boltstream/broker/group_coordinator.h"
#include "boltstream/protocol/protocol.h"
#include "boltstream/storage/offset_store.h"
#include "boltstream/storage/partition_log.h"

#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace boltstream::broker {

namespace {

struct TopicManifest {
  std::string topic;
  std::uint16_t partition_count{0};
  std::string created_at_utc;
};

class BrokerRequestError : public std::runtime_error {
public:
  BrokerRequestError(protocol::ErrorCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}

  [[nodiscard]] protocol::ErrorCode code() const { return code_; }

private:
  protocol::ErrorCode code_;
};

std::filesystem::path topic_directory(const std::filesystem::path& data_dir,
                                      std::string_view topic) {
  return data_dir / "topics" / std::string{topic};
}

std::filesystem::path topic_manifest_path(const std::filesystem::path& data_dir,
                                          std::string_view topic) {
  return topic_directory(data_dir, topic) / "manifest.json";
}

std::string json_escape(std::string_view value) {
  std::ostringstream out;
  for (const char ch : value) {
    switch (ch) {
    case '\\':
      out << "\\\\";
      break;
    case '"':
      out << "\\\"";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      out << ch;
      break;
    }
  }
  return out.str();
}

struct StructuredLogFields {
  StructuredLogFields(std::string level_in, std::string event_in, std::string remote_in = {},
                      std::string frame_type_in = {}, std::string error_code_in = {},
                      std::string message_in = {},
                      std::optional<std::uint64_t> correlation_id_in = std::nullopt,
                      std::optional<std::uint32_t> payload_bytes_in = std::nullopt,
                      std::optional<bool> retryable_in = std::nullopt,
                      std::optional<std::uint32_t> append_queue_depth_in = std::nullopt,
                      std::optional<std::uint32_t> waiter_count_in = std::nullopt,
                      std::optional<std::uint64_t> request_duration_ms_in = std::nullopt)
      : level(std::move(level_in)), event(std::move(event_in)), remote(std::move(remote_in)),
        frame_type(std::move(frame_type_in)), error_code(std::move(error_code_in)),
        message(std::move(message_in)), correlation_id(correlation_id_in),
        payload_bytes(payload_bytes_in), retryable(retryable_in),
        append_queue_depth(append_queue_depth_in), waiter_count(waiter_count_in),
        request_duration_ms(request_duration_ms_in) {}

  std::string level{"info"};
  std::string event;
  std::string remote;
  std::string frame_type;
  std::string error_code;
  std::string message;
  std::optional<std::uint64_t> correlation_id;
  std::optional<std::uint32_t> payload_bytes;
  std::optional<bool> retryable;
  std::optional<std::uint32_t> append_queue_depth;
  std::optional<std::uint32_t> waiter_count;
  std::optional<std::uint64_t> request_duration_ms;
  std::map<std::string, std::string> string_fields;
  std::map<std::string, std::uint64_t> numeric_fields;
};

std::mutex& structured_log_mutex() {
  static std::mutex mutex;
  return mutex;
}

void write_structured_log(const StructuredLogFields& fields) {
  std::ostringstream out;
  out << "{";
  out << "\"timestamp\":\"" << utc_now_iso8601() << "\"";
  out << ",\"level\":\"" << json_escape(fields.level) << "\"";
  out << ",\"event\":\"" << json_escape(fields.event) << "\"";
  if (!fields.remote.empty()) {
    out << ",\"remote\":\"" << json_escape(fields.remote) << "\"";
  }
  if (fields.correlation_id) {
    out << ",\"correlation_id\":" << *fields.correlation_id;
  }
  if (!fields.frame_type.empty()) {
    out << ",\"frame_type\":\"" << json_escape(fields.frame_type) << "\"";
  }
  if (fields.payload_bytes) {
    out << ",\"payload_bytes\":" << *fields.payload_bytes;
  }
  if (!fields.error_code.empty()) {
    out << ",\"error_code\":\"" << json_escape(fields.error_code) << "\"";
  }
  if (fields.retryable) {
    out << ",\"retryable\":" << (*fields.retryable ? "true" : "false");
  }
  if (fields.append_queue_depth) {
    out << ",\"append_queue_depth\":" << *fields.append_queue_depth;
  }
  if (fields.waiter_count) {
    out << ",\"waiter_count\":" << *fields.waiter_count;
  }
  if (fields.request_duration_ms) {
    out << ",\"request_duration_ms\":" << *fields.request_duration_ms;
  }
  for (const auto& [key, value] : fields.string_fields) {
    out << ",\"" << json_escape(key) << "\":\"" << json_escape(value) << "\"";
  }
  for (const auto& [key, value] : fields.numeric_fields) {
    out << ",\"" << json_escape(key) << "\":" << value;
  }
  if (!fields.message.empty()) {
    out << ",\"message\":\"" << json_escape(fields.message) << "\"";
  }
  out << "}\n";

  std::lock_guard lock{structured_log_mutex()};
  std::cerr << out.str();
}

std::string endpoint_string(const boost::asio::ip::tcp::endpoint& endpoint) {
  return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

std::string assignment_string(const std::vector<std::uint16_t>& assignment) {
  std::ostringstream out;
  for (std::size_t index = 0; index < assignment.size(); ++index) {
    if (index > 0) {
      out << ",";
    }
    out << assignment[index];
  }
  return out.str();
}

std::optional<std::string> json_string_field(std::string_view json, std::string_view name) {
  const auto needle = "\"" + std::string{name} + "\"";
  const auto key = json.find(needle);
  if (key == std::string_view::npos) {
    return std::nullopt;
  }
  const auto colon = json.find(':', key + needle.size());
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  const auto first_quote = json.find('"', colon + 1);
  if (first_quote == std::string_view::npos) {
    return std::nullopt;
  }
  const auto second_quote = json.find('"', first_quote + 1);
  if (second_quote == std::string_view::npos) {
    return std::nullopt;
  }
  return std::string{json.substr(first_quote + 1, second_quote - first_quote - 1)};
}

std::optional<std::uint16_t> json_u16_field(std::string_view json, std::string_view name) {
  const auto needle = "\"" + std::string{name} + "\"";
  const auto key = json.find(needle);
  if (key == std::string_view::npos) {
    return std::nullopt;
  }
  const auto colon = json.find(':', key + needle.size());
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  auto begin = colon + 1;
  while (begin < json.size() && (json[begin] == ' ' || json[begin] == '\t')) {
    ++begin;
  }
  auto end = begin;
  while (end < json.size() && json[end] >= '0' && json[end] <= '9') {
    ++end;
  }
  if (begin == end) {
    return std::nullopt;
  }
  unsigned int parsed = 0;
  const auto result = std::from_chars(json.data() + begin, json.data() + end, parsed);
  if (result.ec != std::errc{} || result.ptr != json.data() + end ||
      parsed > std::numeric_limits<std::uint16_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(parsed);
}

std::optional<TopicManifest> read_topic_manifest(const std::filesystem::path& data_dir,
                                                 std::string_view topic) {
  const auto path = topic_manifest_path(data_dir, topic);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return std::nullopt;
  }

  std::ifstream in{path, std::ios::binary};
  if (!in) {
    throw std::runtime_error("failed to open topic manifest: " + path.string());
  }
  const std::string json{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

  auto manifest_topic = json_string_field(json, "topic");
  const auto partition_count = json_u16_field(json, "partition_count");
  auto created_at = json_string_field(json, "created_at_utc");
  if (!manifest_topic || !partition_count || *partition_count == 0) {
    throw std::runtime_error("invalid topic manifest: " + path.string());
  }
  if (*manifest_topic != topic) {
    throw std::runtime_error("topic manifest name mismatch: " + path.string());
  }
  if (!created_at) {
    created_at = "unknown";
  }
  return TopicManifest{*manifest_topic, *partition_count, *created_at};
}

void write_topic_manifest(const std::filesystem::path& data_dir, const TopicManifest& manifest) {
  std::error_code ec;
  std::filesystem::create_directories(topic_directory(data_dir, manifest.topic), ec);
  if (ec) {
    throw std::runtime_error("failed to create topic directory: " + ec.message());
  }

  const auto path = topic_manifest_path(data_dir, manifest.topic);
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  if (!out) {
    throw std::runtime_error("failed to write topic manifest: " + path.string());
  }
  out << "{\n";
  out << "  \"manifest_version\": 1,\n";
  out << "  \"topic\": \"" << json_escape(manifest.topic) << "\",\n";
  out << "  \"partition_count\": " << manifest.partition_count << ",\n";
  out << "  \"created_at_utc\": \"" << json_escape(manifest.created_at_utc) << "\"\n";
  out << "}\n";
  out.flush();
  if (!out) {
    throw std::runtime_error("failed to flush topic manifest: " + path.string());
  }
}

std::uint16_t parse_partition_count_or_migrate(const std::filesystem::path& data_dir,
                                               std::string_view topic) {
  if (const auto manifest = read_topic_manifest(data_dir, topic)) {
    return manifest->partition_count;
  }

  const auto legacy_partition =
      storage::partition_directory(data_dir, topic, storage::kPhaseThreePartition);
  std::error_code ec;
  if (!std::filesystem::exists(legacy_partition, ec)) {
    return 0;
  }

  write_topic_manifest(data_dir, {std::string{topic}, 1, utc_now_iso8601()});
  return 1;
}

std::uint16_t key_hash_partition(std::span<const std::uint8_t> key, std::uint16_t partition_count) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto byte : key) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return static_cast<std::uint16_t>(hash % partition_count);
}

} // namespace

class BrokerRuntime {
  struct AppendJob;

  struct TopicState {
    struct PartitionState {
      explicit PartitionState(storage::PartitionLog opened_log) : log(std::move(opened_log)) {}

      mutable std::mutex log_mutex;
      storage::PartitionLog log;
      mutable std::mutex append_mutex;
      std::deque<std::shared_ptr<AppendJob>> pending_appends;
      bool append_active{false};
    };

    mutable std::mutex mutex;
    std::string name;
    std::uint16_t partition_count{0};
    std::uint16_t next_round_robin_partition{0};
    bool deleting{false};
    std::vector<std::shared_ptr<PartitionState>> partitions;
  };

public:
  struct ProduceResult {
    bool ok{false};
    protocol::ProduceResponse response;
    protocol::ErrorCode error{protocol::ErrorCode::InternalError};
    std::string message;
  };

  using ProduceCompletion = std::function<void(ProduceResult)>;

  explicit BrokerRuntime(ServerOptions options, std::string broker_token)
      : data_dir_(std::move(options.data_dir)), max_frame_bytes_(options.max_frame_bytes),
        max_fetch_records_(options.max_fetch_records), max_fetch_bytes_(options.max_fetch_bytes),
        max_topic_partitions_(options.max_topic_partitions),
        max_fetch_wait_ms_(options.max_fetch_wait_ms),
        max_append_queue_depth_(options.max_append_queue_depth),
        append_workers_configured_(options.append_workers),
        max_long_poll_waiters_(options.max_long_poll_waiters),
        segment_bytes_(options.segment_bytes),
        segment_max_age_seconds_(options.segment_max_age_seconds),
        retention_policy_{options.retention_max_age_seconds, options.retention_max_bytes},
        broker_token_(std::move(broker_token)),
        offset_store_(storage::OffsetStore::open(data_dir_)) {
    start_append_workers();
  }

  ~BrokerRuntime() { stop_append_workers(); }

  storage::StorageRecoverySummary load_existing_topics() {
    storage::StorageRecoverySummary summary;
    const auto topics_dir = data_dir_ / "topics";
    std::error_code ec;
    if (!std::filesystem::exists(topics_dir, ec)) {
      return summary;
    }

    std::lock_guard lock{topics_mutex_};
    for (const auto& entry : std::filesystem::directory_iterator(topics_dir)) {
      if (!entry.is_directory()) {
        continue;
      }

      const auto topic = entry.path().filename().string();
      if (!storage::is_valid_topic_name(topic)) {
        continue;
      }

      const auto partition_count = parse_partition_count_or_migrate(data_dir_, topic);
      if (partition_count == 0) {
        continue;
      }

      auto topic_state = open_topic_state(topic, partition_count);
      for (const auto& partition : topic_state->partitions) {
        const auto& stats = partition->log.recovery_stats();
        summary.segments_scanned += stats.segments_scanned;
        summary.indexes_rebuilt += stats.indexes_rebuilt;
        summary.records_recovered += stats.records_recovered;
        summary.bytes_truncated += stats.bytes_truncated;
      }
      topics_[topic] = std::move(topic_state);

      ++summary.topics_recovered;
      summary.partitions_recovered += partition_count;
    }
    return summary;
  }

  [[nodiscard]] bool auth_required() const { return !broker_token_.empty(); }

  [[nodiscard]] bool authenticate(std::string_view token) const {
    return !auth_required() || token == broker_token_;
  }

  protocol::CreateTopicResponse create_topic(const protocol::CreateTopicRequest& request) {
    if (!storage::is_valid_topic_name(request.topic)) {
      throw BrokerRequestError{protocol::ErrorCode::MalformedPayload, "invalid topic name"};
    }
    if (request.partition_count == 0 || request.partition_count > max_topic_partitions_) {
      throw BrokerRequestError{protocol::ErrorCode::InvalidPartition,
                               "partition count exceeds configured maximum"};
    }

    std::lock_guard lock{topics_mutex_};
    if (const auto existing = topics_.find(request.topic); existing != topics_.end()) {
      if (existing->second->partition_count != request.partition_count) {
        throw BrokerRequestError{protocol::ErrorCode::TopicConflict,
                                 "topic already exists with a different partition count"};
      }
      return {request.topic, request.partition_count, "exists"};
    }

    write_topic_manifest(data_dir_, {request.topic, request.partition_count, utc_now_iso8601()});
    topics_[request.topic] = open_topic_state(request.topic, request.partition_count);
    return {request.topic, request.partition_count, "created"};
  }

  void async_produce(protocol::ProduceRequest request, boost::asio::any_io_executor executor,
                     ProduceCompletion completion) {
    auto topic = find_topic_or_throw(request.topic);

    auto job = std::make_shared<AppendJob>();
    job->request = std::move(request);
    job->executor = std::move(executor);
    job->completion = std::move(completion);

    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }

    const auto partition_id = job->request.key.empty()
                                  ? topic->next_round_robin_partition
                                  : key_hash_partition(job->request.key, topic->partition_count);
    auto partition = topic->partitions.at(partition_id);
    const auto enqueued = try_enqueue_append(partition, job);
    if (!enqueued.accepted) {
      write_structured_log({"warn",
                            "append_overloaded",
                            {},
                            {},
                            std::string{protocol::error_code_name(protocol::ErrorCode::Overloaded)},
                            "append queue is full",
                            std::nullopt,
                            std::nullopt,
                            protocol::is_retryable_error(protocol::ErrorCode::Overloaded),
                            enqueued.depth,
                            active_long_poll_waiters()});
      throw BrokerRequestError{protocol::ErrorCode::Overloaded, "append queue is full"};
    }
    if (job->request.key.empty()) {
      topic->next_round_robin_partition = static_cast<std::uint16_t>(
          (topic->next_round_robin_partition + 1U) % topic->partition_count);
    }
  }

  protocol::FetchResponse fetch(const protocol::FetchRequest& request) const {
    auto topic = find_topic_or_throw(request.topic);
    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }
    auto partition = partition_or_throw(*topic, request.partition);
    std::lock_guard log_lock{partition->log_mutex};
    const auto earliest_offset = partition->log.earliest_offset();
    const auto next_offset = partition->log.next_offset();
    const auto from_offset = resolve_fetch_offset(request, earliest_offset, next_offset);

    protocol::FetchResponse response;
    response.topic = request.topic;
    response.partition = request.partition;
    response.from_offset = from_offset;
    response.next_offset = from_offset;
    const auto max_payload_bytes = max_fetch_payload_bytes();
    auto payload_bytes = fetch_payload_base_bytes(response);
    if (payload_bytes > max_payload_bytes) {
      throw BrokerRequestError{protocol::ErrorCode::InvalidLength,
                               "fetch response metadata exceeds configured maximum size"};
    }
    const auto records =
        partition->log.read_from(from_offset, max_fetch_records_, max_fetch_bytes_);
    response.records.reserve(records.size());
    for (const auto& record : records) {
      const auto record_payload_bytes = fetch_record_payload_bytes(record);
      if (payload_bytes + record_payload_bytes > max_payload_bytes) {
        if (response.records.empty()) {
          throw BrokerRequestError{protocol::ErrorCode::InvalidLength,
                                   "single fetch record exceeds configured maximum response size"};
        }
        break;
      }

      protocol::FetchRecord out;
      out.offset = record.metadata.offset;
      out.timestamp_unix_ns = record.metadata.timestamp_unix_ns;
      out.key = record.key;
      out.message = record.value;
      out.encoded_byte_size = record.metadata.encoded_byte_size;
      response.next_offset = out.offset + 1;
      payload_bytes += record_payload_bytes;
      response.records.push_back(std::move(out));
    }
    return response;
  }

  protocol::OffsetCommitResponse commit_offset(const protocol::OffsetCommitRequest& request) {
    validate_group_or_throw(request.group);
    auto topic = find_topic_or_throw(request.topic);
    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }
    auto partition = partition_or_throw(*topic, request.partition);
    log_group_expirations(group_coordinator_.expire(request.group, request.topic));
    if (group_coordinator_.has_active_members(request.group, request.topic)) {
      throw BrokerRequestError{protocol::ErrorCode::StaleMember,
                               "legacy offset commit is fenced by active coordinated group"};
    }
    std::lock_guard log_lock{partition->log_mutex};
    const auto earliest_offset = partition->log.earliest_offset();
    if (request.next_offset < earliest_offset) {
      throw BrokerRequestError{protocol::ErrorCode::OffsetOutOfRange,
                               "commit offset is below partition earliest offset"};
    }
    if (request.next_offset > partition->log.next_offset()) {
      throw BrokerRequestError{protocol::ErrorCode::InvalidOffset,
                               "commit offset is beyond partition next offset"};
    }

    std::lock_guard offset_lock{offsets_mutex_};
    offset_store_.commit(request.group, request.topic, request.partition, request.next_offset);
    return {request.group, request.topic, request.partition, request.next_offset};
  }

  protocol::JoinGroupResponse join_group(const protocol::JoinGroupRequest& request) {
    validate_group_or_throw(request.group);
    auto topic = find_topic_or_throw(request.topic);
    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }
    GroupCoordinator::Result result;
    try {
      result = group_coordinator_.join(request.group, request.topic, request.member_id,
                                       request.session_timeout_ms, topic->partition_count);
    } catch (const GroupCoordinatorError& error) {
      throw BrokerRequestError{error.code(), error.what()};
    }
    log_expired_members(result);
    if (result.member_joined) {
      log_group_result("group_member_joined", result);
    }
    if (result.generation_changed) {
      log_group_result("group_rebalanced", result);
    }
    return {result.group, result.topic, result.member_id, result.generation_id};
  }

  protocol::SyncGroupResponse sync_group(const protocol::SyncGroupRequest& request) {
    validate_group_or_throw(request.group);
    auto topic = find_topic_or_throw(request.topic);
    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }
    log_group_expirations(group_coordinator_.expire(request.group, request.topic));
    GroupCoordinator::Result result;
    try {
      result = group_coordinator_.sync(request.group, request.topic, request.member_id,
                                       request.generation_id);
    } catch (const GroupCoordinatorError& error) {
      throw BrokerRequestError{error.code(), error.what()};
    }
    return {result.group, result.topic, result.member_id, result.generation_id, result.assignment};
  }

  protocol::HeartbeatResponse heartbeat_group(const protocol::HeartbeatRequest& request) {
    validate_group_or_throw(request.group);
    auto topic = find_topic_or_throw(request.topic);
    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }
    log_group_expirations(group_coordinator_.expire(request.group, request.topic));
    GroupCoordinator::Result result;
    try {
      result = group_coordinator_.heartbeat(request.group, request.topic, request.member_id,
                                            request.generation_id);
    } catch (const GroupCoordinatorError& error) {
      throw BrokerRequestError{error.code(), error.what()};
    }
    log_group_result("group_heartbeat", result);
    return {result.group, result.topic, result.member_id, result.generation_id, "ok"};
  }

  protocol::LeaveGroupResponse leave_group(const protocol::LeaveGroupRequest& request) {
    validate_group_or_throw(request.group);
    auto topic = find_topic_or_throw(request.topic);
    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }
    log_group_expirations(group_coordinator_.expire(request.group, request.topic));
    GroupCoordinator::Result result;
    try {
      result = group_coordinator_.leave(request.group, request.topic, request.member_id,
                                        request.generation_id);
    } catch (const GroupCoordinatorError& error) {
      throw BrokerRequestError{error.code(), error.what()};
    }
    log_group_result("group_member_left", result);
    if (result.generation_changed) {
      log_group_result("group_rebalanced", result);
    }
    return {result.group, result.topic, result.member_id, result.generation_id, "left"};
  }

  protocol::GroupOffsetCommitResponse
  commit_group_offset(const protocol::GroupOffsetCommitRequest& request) {
    validate_group_or_throw(request.group);
    auto topic = find_topic_or_throw(request.topic);
    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }
    auto partition = partition_or_throw(*topic, request.partition);
    std::lock_guard log_lock{partition->log_mutex};
    const auto earliest_offset = partition->log.earliest_offset();
    if (request.next_offset < earliest_offset) {
      log_group_commit_rejected(request, protocol::ErrorCode::OffsetOutOfRange,
                                "commit offset is below partition earliest offset");
      throw BrokerRequestError{protocol::ErrorCode::OffsetOutOfRange,
                               "commit offset is below partition earliest offset"};
    }
    if (request.next_offset > partition->log.next_offset()) {
      log_group_commit_rejected(request, protocol::ErrorCode::InvalidOffset,
                                "commit offset is beyond partition next offset");
      throw BrokerRequestError{protocol::ErrorCode::InvalidOffset,
                               "commit offset is beyond partition next offset"};
    }

    GroupCoordinator::Result result;
    try {
      log_group_expirations(group_coordinator_.expire(request.group, request.topic));
      result = group_coordinator_.validate_commit(request.group, request.topic, request.member_id,
                                                  request.generation_id, request.partition);
    } catch (const GroupCoordinatorError& error) {
      log_group_commit_rejected(request, error.code(), error.what());
      throw BrokerRequestError{error.code(), error.what()};
    }

    std::lock_guard offset_lock{offsets_mutex_};
    offset_store_.commit(request.group, request.topic, request.partition, request.next_offset);
    log_group_commit(request, result);
    return {request.group,        request.topic,     request.member_id,
            result.generation_id, request.partition, request.next_offset};
  }

  protocol::MetadataResponse metadata() const {
    protocol::MetadataResponse response;
    std::lock_guard topics_lock{topics_mutex_};
    for (const auto& [topic_name, topic] : topics_) {
      response.topics.reserve(response.topics.size() + topic->partitions.size());
      for (const auto& partition : topic->partitions) {
        std::lock_guard log_lock{partition->log_mutex};
        response.topics.push_back(
            {topic_name, partition->log.options().partition_id, partition->log.next_offset()});
      }
    }
    return response;
  }

  protocol::ListTopicsResponse list_topics() const {
    protocol::ListTopicsResponse response;
    std::vector<std::shared_ptr<TopicState>> topics;
    {
      std::lock_guard topics_lock{topics_mutex_};
      topics.reserve(topics_.size());
      for (const auto& [_, topic] : topics_) {
        topics.push_back(topic);
      }
    }

    response.topics.reserve(topics.size());
    for (const auto& topic : topics) {
      try {
        response.topics.push_back(describe_topic_state(*topic));
      } catch (const BrokerRequestError&) {
      }
    }
    return response;
  }

  protocol::DescribeTopicResponse
  describe_topic(const protocol::DescribeTopicRequest& request) const {
    if (!storage::is_valid_topic_name(request.topic)) {
      throw BrokerRequestError{protocol::ErrorCode::MalformedPayload, "invalid topic name"};
    }
    const auto topic = find_topic_or_throw(request.topic);
    return {describe_topic_state(*topic)};
  }

  protocol::RunRetentionResponse run_retention(std::string_view topic_filter) {
    if (!topic_filter.empty() && !storage::is_valid_topic_name(topic_filter)) {
      throw BrokerRequestError{protocol::ErrorCode::MalformedPayload, "invalid topic name"};
    }

    protocol::RunRetentionResponse response;
    response.topic = std::string{topic_filter};
    std::vector<std::shared_ptr<TopicState>> topics;
    {
      std::lock_guard topics_lock{topics_mutex_};
      for (const auto& [name, topic] : topics_) {
        if (!topic_filter.empty() && name != topic_filter) {
          continue;
        }
        topics.push_back(topic);
      }
    }
    if (!topic_filter.empty() && topics.empty()) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic does not exist"};
    }

    response.topics_scanned = static_cast<std::uint32_t>(topics.size());
    for (const auto& topic : topics) {
      std::lock_guard topic_lock{topic->mutex};
      if (topic->deleting) {
        continue;
      }
      for (const auto& partition : topic->partitions) {
        std::lock_guard log_lock{partition->log_mutex};
        const auto stats = partition->log.apply_retention(retention_policy_);
        protocol::RetentionPartitionResult result;
        result.topic = topic->name;
        result.partition = partition->log.options().partition_id;
        result.segments_deleted = static_cast<std::uint32_t>(stats.segments_deleted);
        result.bytes_deleted = static_cast<std::uint64_t>(stats.bytes_deleted);
        result.earliest_offset = stats.earliest_offset;
        result.next_offset = stats.next_offset;
        response.partitions.push_back(result);
        ++response.partitions_scanned;
        response.segments_deleted += result.segments_deleted;
        response.bytes_deleted += result.bytes_deleted;
      }
    }

    if (response.segments_deleted > 0) {
      write_structured_log(
          {"info",
           "retention_applied",
           {},
           {},
           {},
           "topics_scanned=" + std::to_string(response.topics_scanned) +
               " partitions_scanned=" + std::to_string(response.partitions_scanned) +
               " segments_deleted=" + std::to_string(response.segments_deleted) +
               " bytes_deleted=" + std::to_string(response.bytes_deleted),
           std::nullopt,
           std::nullopt,
           std::nullopt,
           std::nullopt,
           active_long_poll_waiters()});
    }
    return response;
  }

  protocol::DeleteTopicResponse delete_topic(const protocol::DeleteTopicRequest& request) {
    if (!storage::is_valid_topic_name(request.topic)) {
      throw BrokerRequestError{protocol::ErrorCode::MalformedPayload, "invalid topic name"};
    }
    if (group_coordinator_.topic_has_active_members(request.topic)) {
      throw BrokerRequestError{protocol::ErrorCode::GroupActive,
                               "topic has active coordinated consumer group members"};
    }

    std::shared_ptr<TopicState> topic;
    {
      std::lock_guard topics_lock{topics_mutex_};
      auto it = topics_.find(request.topic);
      if (it == topics_.end()) {
        throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic does not exist"};
      }
      topic = it->second;
      {
        std::lock_guard topic_lock{topic->mutex};
        if (group_coordinator_.topic_has_active_members(request.topic)) {
          throw BrokerRequestError{protocol::ErrorCode::GroupActive,
                                   "topic has active coordinated consumer group members"};
        }
        topic->deleting = true;
      }
      topics_.erase(it);
    }

    notify_fetch_waiters(request.topic);
    wait_for_topic_appends_to_drain(*topic);

    protocol::DeleteTopicResponse response;
    response.topic = request.topic;
    response.status = "deleted";
    response.partitions_deleted = topic->partition_count;
    for (const auto& partition : topic->partitions) {
      std::lock_guard log_lock{partition->log_mutex};
      const auto summaries = partition->log.segment_summaries();
      response.segments_deleted += static_cast<std::uint32_t>(summaries.size());
      for (const auto& summary : summaries) {
        response.bytes_deleted += static_cast<std::uint64_t>(summary.log_bytes);
      }
    }

    std::error_code ec;
    std::filesystem::remove_all(topic_directory(data_dir_, request.topic), ec);
    if (ec) {
      throw BrokerRequestError{protocol::ErrorCode::InternalError,
                               "failed to delete topic directory: " + ec.message()};
    }

    {
      std::lock_guard offset_lock{offsets_mutex_};
      const auto offsets = offset_store_.remove_topic(request.topic);
      response.offsets_removed = static_cast<std::uint32_t>(offsets.offsets_removed);
    }
    group_coordinator_.remove_topic(request.topic);
    write_structured_log({"info",
                          "topic_deleted",
                          {},
                          {},
                          {},
                          "topic=" + request.topic +
                              " partitions_deleted=" + std::to_string(response.partitions_deleted) +
                              " segments_deleted=" + std::to_string(response.segments_deleted) +
                              " bytes_deleted=" + std::to_string(response.bytes_deleted) +
                              " offsets_removed=" + std::to_string(response.offsets_removed),
                          std::nullopt,
                          std::nullopt,
                          std::nullopt,
                          std::nullopt,
                          active_long_poll_waiters()});
    return response;
  }

  protocol::DescribeGroupResponse describe_group(const protocol::DescribeGroupRequest& request) {
    validate_group_or_throw(request.group);
    auto topic = find_topic_or_throw(request.topic);
    protocol::DescribeGroupResponse response;
    response.group = request.group;
    response.topic = request.topic;
    response.active_member_count = static_cast<std::uint32_t>(
        group_coordinator_.active_member_count(request.group, request.topic));

    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }
    response.offsets.reserve(topic->partitions.size());
    for (const auto& partition : topic->partitions) {
      std::lock_guard log_lock{partition->log_mutex};
      protocol::GroupOffsetDescription offset;
      offset.partition = partition->log.options().partition_id;
      offset.earliest_offset = partition->log.earliest_offset();
      offset.next_offset = partition->log.next_offset();
      {
        std::lock_guard offset_lock{offsets_mutex_};
        const auto committed =
            offset_store_.committed(request.group, request.topic, offset.partition);
        if (committed) {
          offset.has_committed_offset = true;
          offset.committed_offset = *committed;
        }
      }
      offset.out_of_range =
          offset.has_committed_offset && (offset.committed_offset < offset.earliest_offset ||
                                          offset.committed_offset > offset.next_offset);
      if (offset.has_committed_offset && !offset.out_of_range &&
          offset.committed_offset <= offset.next_offset) {
        offset.lag = offset.next_offset - offset.committed_offset;
      } else {
        offset.lag = offset.next_offset - offset.earliest_offset;
      }
      response.offsets.push_back(offset);
    }
    return response;
  }

  protocol::ResetGroupOffsetResponse
  reset_group_offset(const protocol::ResetGroupOffsetRequest& request) {
    validate_group_or_throw(request.group);
    if (group_coordinator_.has_active_members(request.group, request.topic)) {
      throw BrokerRequestError{protocol::ErrorCode::GroupActive,
                               "consumer group has active members"};
    }
    auto topic = find_topic_or_throw(request.topic);
    std::lock_guard topic_lock{topic->mutex};
    if (topic->deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }
    if (group_coordinator_.has_active_members(request.group, request.topic)) {
      throw BrokerRequestError{protocol::ErrorCode::GroupActive,
                               "consumer group has active members"};
    }
    auto partition = partition_or_throw(*topic, request.partition);
    std::lock_guard log_lock{partition->log_mutex};
    const auto earliest_offset = partition->log.earliest_offset();
    const auto next_offset = partition->log.next_offset();

    std::uint64_t target = 0;
    if (request.to == "beginning") {
      target = earliest_offset;
    } else if (request.to == "latest") {
      target = next_offset;
    } else if (!parse_u64(request.to, target)) {
      throw BrokerRequestError{protocol::ErrorCode::MalformedPayload,
                               "reset target must be beginning, latest, or an unsigned offset"};
    }
    if (target < earliest_offset) {
      throw BrokerRequestError{protocol::ErrorCode::OffsetOutOfRange,
                               "reset target is below partition earliest offset"};
    }
    if (target > next_offset) {
      throw BrokerRequestError{protocol::ErrorCode::InvalidOffset,
                               "reset target is beyond partition next offset"};
    }

    {
      std::lock_guard offset_lock{offsets_mutex_};
      offset_store_.commit(request.group, request.topic, request.partition, target);
    }
    return {request.group, request.topic, request.partition, target, "reset"};
  }

  [[nodiscard]] std::uint32_t max_frame_bytes() const { return max_frame_bytes_; }
  [[nodiscard]] std::uint32_t max_fetch_wait_ms() const { return max_fetch_wait_ms_; }

  std::optional<std::uint64_t> register_fetch_waiter(std::string topic, std::uint16_t partition,
                                                     std::function<void()> callback) {
    std::lock_guard lock{waiters_mutex_};
    if (active_long_poll_waiters_ >= max_long_poll_waiters_) {
      return std::nullopt;
    }
    const auto id = next_waiter_id_++;
    waiters_[{std::move(topic), partition}].push_back({id, std::move(callback)});
    ++active_long_poll_waiters_;
    return id;
  }

  void cancel_fetch_waiter(std::uint64_t waiter_id) {
    std::lock_guard lock{waiters_mutex_};
    for (auto it = waiters_.begin(); it != waiters_.end();) {
      auto& callbacks = it->second;
      const auto before = callbacks.size();
      callbacks.erase(
          std::remove_if(callbacks.begin(), callbacks.end(),
                         [waiter_id](const auto& waiter) { return waiter.id == waiter_id; }),
          callbacks.end());
      const auto removed = before - callbacks.size();
      active_long_poll_waiters_ -=
          static_cast<std::uint32_t>(std::min<std::size_t>(removed, active_long_poll_waiters_));
      if (callbacks.empty()) {
        it = waiters_.erase(it);
      } else {
        ++it;
      }
    }
  }

  [[nodiscard]] std::uint32_t active_long_poll_waiters() const {
    std::lock_guard lock{waiters_mutex_};
    return active_long_poll_waiters_;
  }

private:
  struct WaiterKey {
    std::string topic;
    std::uint16_t partition{0};

    bool operator<(const WaiterKey& other) const {
      if (topic != other.topic) {
        return topic < other.topic;
      }
      return partition < other.partition;
    }
  };

  struct FetchWaiter {
    std::uint64_t id{0};
    std::function<void()> callback;
  };

  struct AppendJob {
    protocol::ProduceRequest request;
    boost::asio::any_io_executor executor;
    ProduceCompletion completion;
  };

  struct ReadyAppend {
    std::shared_ptr<TopicState::PartitionState> partition;
    std::shared_ptr<AppendJob> job;
  };

  struct EnqueueOutcome {
    bool accepted{false};
    std::uint32_t depth{0};
  };

  [[nodiscard]] std::size_t max_fetch_payload_bytes() const {
    if (max_frame_bytes_ <= protocol::kFrameHeaderBytes) {
      return 0;
    }
    return static_cast<std::size_t>(
        std::min(max_fetch_bytes_, max_frame_bytes_ - protocol::kFrameHeaderBytes));
  }

  [[nodiscard]] static std::size_t
  fetch_payload_base_bytes(const protocol::FetchResponse& response) {
    return sizeof(std::uint32_t) + response.topic.size() + sizeof(std::uint16_t) +
           sizeof(std::uint64_t) + sizeof(std::uint64_t) + sizeof(std::uint32_t);
  }

  [[nodiscard]] static std::size_t fetch_record_payload_bytes(const storage::Record& record) {
    return sizeof(std::uint64_t) + sizeof(std::uint64_t) + sizeof(std::uint32_t) +
           record.key.size() + sizeof(std::uint32_t) + record.value.size() + sizeof(std::uint32_t);
  }

  [[nodiscard]] EnqueueOutcome
  try_enqueue_append(const std::shared_ptr<TopicState::PartitionState>& partition,
                     std::shared_ptr<AppendJob> job) {
    std::shared_ptr<AppendJob> ready_job;
    {
      std::lock_guard lock{partition->append_mutex};
      const auto current_depth = static_cast<std::uint32_t>(partition->pending_appends.size() +
                                                            (partition->append_active ? 1U : 0U));
      if (current_depth >= max_append_queue_depth_) {
        return {false, current_depth};
      }
      const auto accepted_depth = current_depth + 1U;
      if (partition->append_active) {
        partition->pending_appends.push_back(std::move(job));
        return {true, accepted_depth};
      }
      partition->append_active = true;
      ready_job = std::move(job);
    }

    schedule_append(partition, std::move(ready_job));
    return {true, 1};
  }

  void schedule_append(const std::shared_ptr<TopicState::PartitionState>& partition,
                       std::shared_ptr<AppendJob> job) {
    {
      std::lock_guard lock{append_work_mutex_};
      ready_appends_.push_back({partition, std::move(job)});
    }
    append_work_cv_.notify_one();
  }

  void start_append_workers() {
    append_workers_.reserve(append_workers_configured_);
    for (std::uint32_t worker = 0; worker < append_workers_configured_; ++worker) {
      append_workers_.emplace_back([this, worker] { append_worker_loop(worker); });
    }
  }

  void stop_append_workers() {
    {
      std::lock_guard lock{append_work_mutex_};
      append_stopping_ = true;
    }
    append_work_cv_.notify_all();
    for (auto& worker : append_workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void append_worker_loop(std::uint32_t worker_id) {
    (void)worker_id;
    for (;;) {
      std::shared_ptr<TopicState::PartitionState> partition;
      std::shared_ptr<AppendJob> job;
      {
        std::unique_lock lock{append_work_mutex_};
        append_work_cv_.wait(lock, [this] { return append_stopping_ || !ready_appends_.empty(); });
        if (append_stopping_ && ready_appends_.empty()) {
          return;
        }
        auto ready = std::move(ready_appends_.front());
        ready_appends_.pop_front();
        partition = std::move(ready.partition);
        job = std::move(ready.job);
      }

      auto result = run_append(partition, *job);
      boost::asio::post(job->executor,
                        [completion = std::move(job->completion),
                         result = std::move(result)]() mutable { completion(std::move(result)); });
      complete_append(partition);
    }
  }

  ProduceResult run_append(const std::shared_ptr<TopicState::PartitionState>& partition,
                           const AppendJob& job) {
    ProduceResult result;
    try {
      std::lock_guard log_lock{partition->log_mutex};
      const auto metadata = partition->log.append(job.request.key, job.request.message);
      result.ok = true;
      result.response.topic = metadata.topic;
      result.response.partition = metadata.partition;
      result.response.offset = metadata.offset;
      result.response.next_offset = partition->log.next_offset();
      result.response.encoded_byte_size = metadata.encoded_byte_size;
      const auto retention = partition->log.apply_retention(retention_policy_);
      if (retention.segments_deleted > 0) {
        write_structured_log({"info",
                              "retention_applied",
                              {},
                              {},
                              {},
                              "segments_deleted=" + std::to_string(retention.segments_deleted) +
                                  " bytes_deleted=" + std::to_string(retention.bytes_deleted),
                              std::nullopt,
                              std::nullopt,
                              std::nullopt,
                              std::nullopt,
                              active_long_poll_waiters()});
      }
    } catch (const std::exception& error) {
      result.ok = false;
      result.error = protocol::ErrorCode::InternalError;
      result.message = error.what();
    }

    if (result.ok) {
      notify_fetch_waiters(result.response.topic, result.response.partition);
    } else {
      write_structured_log({"error",
                            "append_failed",
                            {},
                            {},
                            std::string{protocol::error_code_name(result.error)},
                            result.message,
                            std::nullopt,
                            std::nullopt,
                            protocol::is_retryable_error(result.error),
                            std::nullopt,
                            active_long_poll_waiters()});
    }
    return result;
  }

  void complete_append(const std::shared_ptr<TopicState::PartitionState>& partition) {
    std::shared_ptr<AppendJob> next;
    {
      std::lock_guard lock{partition->append_mutex};
      if (!partition->pending_appends.empty()) {
        next = std::move(partition->pending_appends.front());
        partition->pending_appends.pop_front();
      } else {
        partition->append_active = false;
      }
    }
    if (next) {
      schedule_append(partition, std::move(next));
    }
  }

  protocol::TopicDescription describe_topic_state(const TopicState& topic) const {
    std::lock_guard topic_lock{topic.mutex};
    if (topic.deleting) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic is being deleted"};
    }

    protocol::TopicDescription description;
    description.topic = topic.name;
    description.partition_count = topic.partition_count;
    description.partitions.reserve(topic.partitions.size());
    for (const auto& partition : topic.partitions) {
      std::lock_guard log_lock{partition->log_mutex};
      protocol::TopicPartitionDescription partition_description;
      partition_description.partition = partition->log.options().partition_id;
      partition_description.earliest_offset = partition->log.earliest_offset();
      partition_description.next_offset = partition->log.next_offset();
      const auto summaries = partition->log.segment_summaries();
      partition_description.segment_count = static_cast<std::uint32_t>(summaries.size());
      for (const auto& summary : summaries) {
        partition_description.log_bytes += static_cast<std::uint64_t>(summary.log_bytes);
      }
      description.log_bytes += partition_description.log_bytes;
      description.partitions.push_back(partition_description);
    }
    return description;
  }

  static void wait_for_topic_appends_to_drain(const TopicState& topic) {
    for (;;) {
      bool drained = true;
      for (const auto& partition : topic.partitions) {
        std::lock_guard lock{partition->append_mutex};
        if (partition->append_active || !partition->pending_appends.empty()) {
          drained = false;
          break;
        }
      }
      if (drained) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  std::shared_ptr<TopicState> open_topic_state(std::string_view topic_name,
                                               std::uint16_t partition_count) const {
    auto topic = std::make_shared<TopicState>();
    topic->name = std::string{topic_name};
    topic->partition_count = partition_count;
    topic->partitions.reserve(partition_count);
    for (std::uint16_t partition = 0; partition < partition_count; ++partition) {
      auto log = storage::PartitionLog::open(
          {data_dir_, topic->name, partition, segment_bytes_, segment_max_age_seconds_});
      topic->partitions.push_back(std::make_shared<TopicState::PartitionState>(std::move(log)));
    }
    return topic;
  }

  [[nodiscard]] std::shared_ptr<TopicState> find_topic(std::string_view topic_name) const {
    std::lock_guard lock{topics_mutex_};
    const auto existing = topics_.find(std::string{topic_name});
    return existing == topics_.end() ? nullptr : existing->second;
  }

  [[nodiscard]] std::shared_ptr<TopicState> find_topic_or_throw(std::string_view topic_name) const {
    const auto topic = find_topic(topic_name);
    if (!topic) {
      throw BrokerRequestError{protocol::ErrorCode::UnknownTopic, "topic does not exist"};
    }
    return topic;
  }

  [[nodiscard]] static std::shared_ptr<TopicState::PartitionState>
  partition_or_throw(const TopicState& topic, std::uint16_t partition) {
    if (partition >= topic.partition_count) {
      throw BrokerRequestError{protocol::ErrorCode::InvalidPartition,
                               "partition does not exist for topic"};
    }
    return topic.partitions.at(partition);
  }

  [[nodiscard]] std::uint16_t select_partition(TopicState& topic,
                                               std::span<const std::uint8_t> key) const {
    if (!key.empty()) {
      return key_hash_partition(key, topic.partition_count);
    }
    std::lock_guard lock{topic.mutex};
    const auto partition = topic.next_round_robin_partition;
    topic.next_round_robin_partition =
        static_cast<std::uint16_t>((topic.next_round_robin_partition + 1U) % topic.partition_count);
    return partition;
  }

  [[nodiscard]] std::uint64_t resolve_fetch_offset(const protocol::FetchRequest& request,
                                                   std::uint64_t earliest_offset,
                                                   std::uint64_t next_offset) const {
    if (request.from == "beginning") {
      return earliest_offset;
    }
    if (request.from == "latest") {
      return next_offset;
    }
    if (request.from == "committed") {
      validate_group_or_throw(request.group);
      std::lock_guard offset_lock{offsets_mutex_};
      const auto committed =
          offset_store_.committed(request.group, request.topic, request.partition)
              .value_or(earliest_offset);
      if (committed < earliest_offset) {
        throw BrokerRequestError{protocol::ErrorCode::OffsetOutOfRange,
                                 "committed offset is below partition earliest offset"};
      }
      if (committed > next_offset) {
        throw BrokerRequestError{protocol::ErrorCode::InvalidOffset,
                                 "committed offset is beyond partition next offset"};
      }
      return committed;
    }

    std::uint64_t from_offset = 0;
    if (!parse_u64(request.from, from_offset)) {
      throw BrokerRequestError{
          protocol::ErrorCode::MalformedPayload,
          "fetch offset selector must be beginning, latest, committed, or an unsigned offset"};
    }
    if (from_offset < earliest_offset) {
      throw BrokerRequestError{protocol::ErrorCode::OffsetOutOfRange,
                               "fetch offset is below partition earliest offset"};
    }
    if (from_offset > next_offset) {
      throw BrokerRequestError{protocol::ErrorCode::InvalidOffset,
                               "fetch offset is beyond partition next offset"};
    }
    return from_offset;
  }

  static void validate_group_or_throw(std::string_view group) {
    if (!storage::is_valid_group_name(group)) {
      throw BrokerRequestError{protocol::ErrorCode::InvalidGroup, "invalid consumer group name"};
    }
  }

  void notify_fetch_waiters(std::string_view topic, std::uint16_t partition) {
    std::vector<FetchWaiter> waiters;
    {
      std::lock_guard lock{waiters_mutex_};
      const auto existing = waiters_.find({std::string{topic}, partition});
      if (existing == waiters_.end()) {
        return;
      }
      waiters = std::move(existing->second);
      active_long_poll_waiters_ -= static_cast<std::uint32_t>(
          std::min<std::size_t>(waiters.size(), active_long_poll_waiters_));
      waiters_.erase(existing);
    }
    for (auto& waiter : waiters) {
      waiter.callback();
    }
  }

  void notify_fetch_waiters(std::string_view topic) {
    std::vector<FetchWaiter> waiters;
    {
      std::lock_guard lock{waiters_mutex_};
      for (auto it = waiters_.begin(); it != waiters_.end();) {
        if (it->first.topic != topic) {
          ++it;
          continue;
        }
        waiters.insert(waiters.end(), std::make_move_iterator(it->second.begin()),
                       std::make_move_iterator(it->second.end()));
        active_long_poll_waiters_ -= static_cast<std::uint32_t>(
            std::min<std::size_t>(it->second.size(), active_long_poll_waiters_));
        it = waiters_.erase(it);
      }
    }
    for (auto& waiter : waiters) {
      waiter.callback();
    }
  }

  static bool parse_u64(std::string_view text, std::uint64_t& value) {
    if (text.empty()) {
      return false;
    }
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
  }

  void log_group_expirations(const GroupCoordinator::Result& result) {
    log_expired_members(result);
    if (result.generation_changed) {
      log_group_result("group_rebalanced", result);
    }
  }

  void log_expired_members(const GroupCoordinator::Result& result) {
    for (const auto& member_id : result.expired_member_ids) {
      auto fields = group_log_fields("group_member_expired", result);
      fields.string_fields["member_id"] = member_id;
      write_structured_log(fields);
    }
  }

  static StructuredLogFields group_log_fields(std::string event,
                                              const GroupCoordinator::Result& result) {
    StructuredLogFields fields{"info", std::move(event)};
    fields.string_fields["group"] = result.group;
    fields.string_fields["topic"] = result.topic;
    if (!result.member_id.empty()) {
      fields.string_fields["member_id"] = result.member_id;
    }
    fields.string_fields["assignment"] = assignment_string(result.assignment);
    fields.numeric_fields["generation_id"] = result.generation_id;
    return fields;
  }

  void log_group_result(std::string event, const GroupCoordinator::Result& result) {
    write_structured_log(group_log_fields(std::move(event), result));
  }

  void log_group_commit(const protocol::GroupOffsetCommitRequest& request,
                        const GroupCoordinator::Result& result) {
    auto fields = group_log_fields("group_offset_committed", result);
    fields.numeric_fields["partition"] = request.partition;
    fields.numeric_fields["next_offset"] = request.next_offset;
    write_structured_log(fields);
  }

  void log_group_commit_rejected(const protocol::GroupOffsetCommitRequest& request,
                                 protocol::ErrorCode error_code, std::string_view message) {
    StructuredLogFields fields{"warn", "group_offset_commit_rejected"};
    fields.error_code = std::string{protocol::error_code_name(error_code)};
    fields.message = std::string{message};
    fields.retryable = protocol::is_retryable_error(error_code);
    fields.string_fields["group"] = request.group;
    fields.string_fields["topic"] = request.topic;
    fields.string_fields["member_id"] = request.member_id;
    fields.numeric_fields["generation_id"] = request.generation_id;
    fields.numeric_fields["partition"] = request.partition;
    fields.numeric_fields["next_offset"] = request.next_offset;
    write_structured_log(fields);
  }

  std::filesystem::path data_dir_;
  std::uint32_t max_frame_bytes_;
  std::uint32_t max_fetch_records_;
  std::uint32_t max_fetch_bytes_;
  std::uint32_t max_topic_partitions_;
  std::uint32_t max_fetch_wait_ms_;
  std::uint32_t max_append_queue_depth_;
  std::uint32_t append_workers_configured_;
  std::uint32_t max_long_poll_waiters_;
  std::uintmax_t segment_bytes_;
  std::uint64_t segment_max_age_seconds_;
  storage::RetentionPolicy retention_policy_;
  std::string broker_token_;
  mutable std::mutex topics_mutex_;
  std::map<std::string, std::shared_ptr<TopicState>> topics_;
  mutable std::mutex offsets_mutex_;
  storage::OffsetStore offset_store_;
  GroupCoordinator group_coordinator_;
  std::mutex append_work_mutex_;
  std::condition_variable append_work_cv_;
  std::deque<ReadyAppend> ready_appends_;
  bool append_stopping_{false};
  std::vector<std::thread> append_workers_;
  mutable std::mutex waiters_mutex_;
  std::uint64_t next_waiter_id_{1};
  std::uint32_t active_long_poll_waiters_{0};
  std::map<WaiterKey, std::vector<FetchWaiter>> waiters_;
};

namespace {

std::string normalize_request_path(std::string_view request) {
  const auto line_end = request.find("\r\n");
  const auto line = request.substr(0, line_end);
  const auto first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    return {};
  }
  const auto second_space = line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos) {
    return {};
  }
  return std::string{line.substr(first_space + 1, second_space - first_space - 1)};
}

std::string broker_token_from_environment() {
#if defined(_WIN32)
  char* token = nullptr;
  std::size_t token_size = 0;
  if (_dupenv_s(&token, &token_size, "BOLTSTREAM_BROKER_TOKEN") != 0 || token == nullptr) {
    return {};
  }
  std::string value{token};
  std::free(token);
  return value;
#else
  const auto* token = std::getenv("BOLTSTREAM_BROKER_TOKEN");
  return token == nullptr ? std::string{} : std::string{token};
#endif
}

class BrokerProtocolSession : public std::enable_shared_from_this<BrokerProtocolSession> {
public:
  using Tcp = boost::asio::ip::tcp;

  BrokerProtocolSession(Tcp::socket socket, BrokerRuntime& runtime, std::function<bool()> ready,
                        std::function<std::string()> ready_detail, std::function<void()> on_close)
      : socket_(std::move(socket)), runtime_(runtime), ready_(std::move(ready)),
        ready_detail_(std::move(ready_detail)), on_close_(std::move(on_close)),
        authenticated_(!runtime_.auth_required()) {
    boost::system::error_code ec;
    const auto endpoint = socket_.remote_endpoint(ec);
    if (!ec) {
      remote_endpoint_ = endpoint_string(endpoint);
    }
  }

  ~BrokerProtocolSession() {
    cancel_active_waiter();
    if (on_close_) {
      on_close_();
    }
  }

  void start() { read_header(); }

private:
  void read_header() {
    auto self = shared_from_this();
    boost::asio::async_read(
        socket_, boost::asio::buffer(header_buffer_),
        [this, self](const boost::system::error_code& ec, std::size_t) {
          if (ec) {
            return;
          }

          const auto decoded = protocol::decode_header(header_buffer_, runtime_.max_frame_bytes());
          if (!decoded.ok) {
            request_started_ = std::chrono::steady_clock::now();
            request_frame_type_ = decoded.header.frame_type;
            write_error(decoded.header.correlation_id, decoded.error, decoded.message, true);
            return;
          }
          read_payload(decoded.header);
        });
  }

  void read_payload(protocol::FrameHeader header) {
    auto self = shared_from_this();
    payload_buffer_.assign(header.payload_bytes, 0);
    boost::asio::async_read(
        socket_, boost::asio::buffer(payload_buffer_),
        [this, self, header](const boost::system::error_code& ec, std::size_t) mutable {
          if (ec) {
            return;
          }

          protocol::Frame frame;
          frame.header = header;
          frame.payload = std::move(payload_buffer_);
          handle_frame(std::move(frame));
        });
  }

  void handle_frame(protocol::Frame frame) {
    request_started_ = std::chrono::steady_clock::now();
    request_frame_type_ = frame.header.frame_type;
    write_structured_log({"info",
                          "protocol_request",
                          remote_endpoint_,
                          std::string{protocol::frame_type_name(frame.header.frame_type)},
                          {},
                          {},
                          frame.header.correlation_id,
                          frame.header.payload_bytes,
                          std::nullopt,
                          std::nullopt,
                          runtime_.active_long_poll_waiters()});

    if (!protocol::is_request_type(frame.header.frame_type)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::UnsupportedRequest,
                  "frame type is not a client request", false);
      return;
    }

    switch (frame.header.frame_type) {
    case protocol::FrameType::HealthRequest:
      handle_health(std::move(frame));
      return;
    case protocol::FrameType::MetadataRequest:
      handle_metadata(std::move(frame));
      return;
    case protocol::FrameType::ProduceRequest:
      handle_produce(std::move(frame));
      return;
    case protocol::FrameType::FetchRequest:
      handle_fetch(std::move(frame));
      return;
    case protocol::FrameType::OffsetCommitRequest:
      handle_offset_commit(std::move(frame));
      return;
    case protocol::FrameType::AuthRequest:
      handle_auth(std::move(frame));
      return;
    case protocol::FrameType::CreateTopicRequest:
      handle_create_topic(std::move(frame));
      return;
    case protocol::FrameType::JoinGroupRequest:
      handle_join_group(std::move(frame));
      return;
    case protocol::FrameType::SyncGroupRequest:
      handle_sync_group(std::move(frame));
      return;
    case protocol::FrameType::HeartbeatRequest:
      handle_heartbeat(std::move(frame));
      return;
    case protocol::FrameType::LeaveGroupRequest:
      handle_leave_group(std::move(frame));
      return;
    case protocol::FrameType::GroupOffsetCommitRequest:
      handle_group_offset_commit(std::move(frame));
      return;
    case protocol::FrameType::ListTopicsRequest:
      handle_list_topics(std::move(frame));
      return;
    case protocol::FrameType::DescribeTopicRequest:
      handle_describe_topic(std::move(frame));
      return;
    case protocol::FrameType::DeleteTopicRequest:
      handle_delete_topic(std::move(frame));
      return;
    case protocol::FrameType::RunRetentionRequest:
      handle_run_retention(std::move(frame));
      return;
    case protocol::FrameType::DescribeGroupRequest:
      handle_describe_group(std::move(frame));
      return;
    case protocol::FrameType::ResetGroupOffsetRequest:
      handle_reset_group_offset(std::move(frame));
      return;
    default:
      write_error(frame.header.correlation_id, protocol::ErrorCode::UnsupportedRequest,
                  "unsupported request frame type", false);
      return;
    }
  }

  void handle_health(protocol::Frame frame) {
    const auto validation = protocol::validate_empty_payload(frame.payload);
    if (!validation.ok) {
      write_error(frame.header.correlation_id, validation.error, validation.message, true);
      return;
    }
    const auto payload =
        protocol::encode_health_response(ready_() ? "ready" : "not_ready", ready_detail_());
    write_frame(protocol::FrameType::HealthResponse, frame.header.correlation_id, payload, false);
  }

  void handle_auth(protocol::Frame frame) {
    protocol::AuthRequest request;
    const auto decoded = protocol::decode_auth_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }

    if (!runtime_.authenticate(request.token)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::Unauthorized,
                  "invalid broker token", true);
      return;
    }

    authenticated_ = true;
    const auto payload =
        protocol::encode_auth_response(runtime_.auth_required() ? "authenticated" : "disabled");
    write_frame(protocol::FrameType::AuthResponse, frame.header.correlation_id, payload, false);
  }

  bool require_auth(std::uint64_t correlation_id) {
    if (authenticated_) {
      return true;
    }
    write_error(correlation_id, protocol::ErrorCode::Unauthorized, "authentication required", true);
    return false;
  }

  void handle_metadata(protocol::Frame frame) {
    const auto validation = protocol::validate_empty_payload(frame.payload);
    if (!validation.ok) {
      write_error(frame.header.correlation_id, validation.error, validation.message, true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    const auto metadata = runtime_.metadata();
    const auto payload = protocol::encode_metadata_response(metadata.topics);
    write_frame(protocol::FrameType::MetadataResponse, frame.header.correlation_id, payload, false);
  }

  void handle_create_topic(protocol::Frame frame) {
    protocol::CreateTopicRequest request;
    const auto decoded = protocol::decode_create_topic_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.create_topic(request);
      const auto payload = protocol::encode_create_topic_response(response);
      write_frame(protocol::FrameType::CreateTopicResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_produce(protocol::Frame frame) {
    protocol::ProduceRequest request;
    const auto decoded = protocol::decode_produce_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto correlation_id = frame.header.correlation_id;
      auto self = shared_from_this();
      runtime_.async_produce(
          std::move(request), socket_.get_executor(),
          [this, self, correlation_id](BrokerRuntime::ProduceResult result) mutable {
            if (!result.ok) {
              write_error(correlation_id, result.error, result.message, false);
              return;
            }
            const auto payload = protocol::encode_produce_response(result.response);
            write_frame(protocol::FrameType::ProduceResponse, correlation_id, payload, false);
          });
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_fetch(protocol::Frame frame) {
    protocol::FetchRequest request;
    const auto decoded = protocol::decode_fetch_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!request.group.empty() && !storage::is_valid_group_name(request.group)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InvalidGroup,
                  "invalid consumer group name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }
    if (request.max_wait_ms > runtime_.max_fetch_wait_ms()) {
      request.max_wait_ms = runtime_.max_fetch_wait_ms();
    }

    fetch_or_wait(frame.header.correlation_id, std::move(request));
  }

  void fetch_or_wait(std::uint64_t correlation_id, protocol::FetchRequest request) {
    try {
      const auto response = runtime_.fetch(request);
      if (response.records.empty() && request.max_wait_ms > 0 &&
          response.from_offset == response.next_offset) {
        register_long_poll(correlation_id, std::move(request), response.from_offset);
        return;
      }
      write_fetch_response(correlation_id, response);
    } catch (const BrokerRequestError& error) {
      write_error(correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(correlation_id, protocol::ErrorCode::InternalError, error.what(), false);
    }
  }

  void register_long_poll(std::uint64_t correlation_id, protocol::FetchRequest request,
                          std::uint64_t from_offset) {
    if (active_waiter_id_) {
      write_error(correlation_id, protocol::ErrorCode::InternalError,
                  "session already has an active long-poll waiter", false);
      return;
    }

    auto self = shared_from_this();
    auto completed = std::make_shared<std::atomic_bool>(false);
    auto timer = std::make_shared<boost::asio::steady_timer>(socket_.get_executor());
    const auto executor = socket_.get_executor();
    const auto wait_ms = request.max_wait_ms;
    request.max_wait_ms = 0;
    request.from = std::to_string(from_offset);

    const auto waiter_id = runtime_.register_fetch_waiter(
        request.topic, request.partition,
        [this, self, executor, completed, timer, correlation_id, request]() mutable {
          boost::asio::post(executor, [this, self, completed, timer, correlation_id,
                                       request]() mutable {
            if (completed->exchange(true)) {
              return;
            }
            active_waiter_id_.reset();
            timer->cancel();
            try {
              const auto response = runtime_.fetch(request);
              write_fetch_response(correlation_id, response);
            } catch (const BrokerRequestError& error) {
              write_error(correlation_id, error.code(), error.what(), false);
            } catch (const std::exception& error) {
              write_error(correlation_id, protocol::ErrorCode::InternalError, error.what(), false);
            }
          });
        });
    if (!waiter_id) {
      write_structured_log(
          {"warn", "long_poll_overloaded", remote_endpoint_,
           std::string{protocol::frame_type_name(protocol::FrameType::FetchRequest)},
           std::string{protocol::error_code_name(protocol::ErrorCode::Overloaded)},
           "long-poll waiter limit reached", correlation_id, std::nullopt,
           protocol::is_retryable_error(protocol::ErrorCode::Overloaded), std::nullopt,
           runtime_.active_long_poll_waiters()});
      write_error(correlation_id, protocol::ErrorCode::Overloaded, "long-poll waiter limit reached",
                  false);
      return;
    }
    active_waiter_id_ = *waiter_id;

    timer->expires_after(std::chrono::milliseconds(wait_ms));
    timer->async_wait([this, self, completed, correlation_id, request,
                       waiter_id](const boost::system::error_code& ec) mutable {
      if (ec || completed->exchange(true)) {
        return;
      }
      runtime_.cancel_fetch_waiter(*waiter_id);
      active_waiter_id_.reset();
      try {
        const auto response = runtime_.fetch(request);
        write_fetch_response(correlation_id, response);
      } catch (const BrokerRequestError& error) {
        write_error(correlation_id, error.code(), error.what(), false);
      } catch (const std::exception& error) {
        write_error(correlation_id, protocol::ErrorCode::InternalError, error.what(), false);
      }
    });
  }

  void write_fetch_response(std::uint64_t correlation_id, protocol::FetchResponse response) {
    auto payload = protocol::encode_fetch_response(response);
    const auto max_payload_bytes =
        runtime_.max_frame_bytes() <= protocol::kFrameHeaderBytes
            ? std::size_t{0}
            : static_cast<std::size_t>(runtime_.max_frame_bytes() - protocol::kFrameHeaderBytes);
    if (payload.size() > max_payload_bytes) {
      write_error(correlation_id, protocol::ErrorCode::InvalidLength,
                  "fetch response exceeds configured maximum frame size", false);
      return;
    }

    write_frame(protocol::FrameType::FetchResponse, correlation_id, payload, false);
  }

  void handle_offset_commit(protocol::Frame frame) {
    protocol::OffsetCommitRequest request;
    const auto decoded = protocol::decode_offset_commit_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_group_name(request.group)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InvalidGroup,
                  "invalid consumer group name", true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.commit_offset(request);
      const auto payload = protocol::encode_offset_commit_response(response);
      write_frame(protocol::FrameType::OffsetCommitResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_join_group(protocol::Frame frame) {
    protocol::JoinGroupRequest request;
    const auto decoded = protocol::decode_join_group_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_group_name(request.group)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InvalidGroup,
                  "invalid consumer group name", true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.join_group(request);
      const auto payload = protocol::encode_join_group_response(response);
      write_frame(protocol::FrameType::JoinGroupResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_sync_group(protocol::Frame frame) {
    protocol::SyncGroupRequest request;
    const auto decoded = protocol::decode_sync_group_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_group_name(request.group)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InvalidGroup,
                  "invalid consumer group name", true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.sync_group(request);
      const auto payload = protocol::encode_sync_group_response(response);
      write_frame(protocol::FrameType::SyncGroupResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_heartbeat(protocol::Frame frame) {
    protocol::HeartbeatRequest request;
    const auto decoded = protocol::decode_heartbeat_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_group_name(request.group)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InvalidGroup,
                  "invalid consumer group name", true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.heartbeat_group(request);
      const auto payload = protocol::encode_heartbeat_response(response);
      write_frame(protocol::FrameType::HeartbeatResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_leave_group(protocol::Frame frame) {
    protocol::LeaveGroupRequest request;
    const auto decoded = protocol::decode_leave_group_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_group_name(request.group)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InvalidGroup,
                  "invalid consumer group name", true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.leave_group(request);
      const auto payload = protocol::encode_leave_group_response(response);
      write_frame(protocol::FrameType::LeaveGroupResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_group_offset_commit(protocol::Frame frame) {
    protocol::GroupOffsetCommitRequest request;
    const auto decoded = protocol::decode_group_offset_commit_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_group_name(request.group)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InvalidGroup,
                  "invalid consumer group name", true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.commit_group_offset(request);
      const auto payload = protocol::encode_group_offset_commit_response(response);
      write_frame(protocol::FrameType::GroupOffsetCommitResponse, frame.header.correlation_id,
                  payload, false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_list_topics(protocol::Frame frame) {
    const auto validation = protocol::validate_empty_payload(frame.payload);
    if (!validation.ok) {
      write_error(frame.header.correlation_id, validation.error, validation.message, true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.list_topics();
      const auto payload = protocol::encode_list_topics_response(response);
      write_frame(protocol::FrameType::ListTopicsResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_describe_topic(protocol::Frame frame) {
    protocol::DescribeTopicRequest request;
    const auto decoded = protocol::decode_describe_topic_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.describe_topic(request);
      const auto payload = protocol::encode_describe_topic_response(response);
      write_frame(protocol::FrameType::DescribeTopicResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_delete_topic(protocol::Frame frame) {
    protocol::DeleteTopicRequest request;
    const auto decoded = protocol::decode_delete_topic_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.delete_topic(request);
      const auto payload = protocol::encode_delete_topic_response(response);
      write_frame(protocol::FrameType::DeleteTopicResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_run_retention(protocol::Frame frame) {
    protocol::RunRetentionRequest request;
    const auto decoded = protocol::decode_run_retention_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!request.topic.empty() && !storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.run_retention(request.topic);
      const auto payload = protocol::encode_run_retention_response(response);
      write_frame(protocol::FrameType::RunRetentionResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_describe_group(protocol::Frame frame) {
    protocol::DescribeGroupRequest request;
    const auto decoded = protocol::decode_describe_group_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_group_name(request.group)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InvalidGroup,
                  "invalid consumer group name", true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.describe_group(request);
      const auto payload = protocol::encode_describe_group_response(response);
      write_frame(protocol::FrameType::DescribeGroupResponse, frame.header.correlation_id, payload,
                  false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_reset_group_offset(protocol::Frame frame) {
    protocol::ResetGroupOffsetRequest request;
    const auto decoded = protocol::decode_reset_group_offset_request(frame.payload, request);
    if (!decoded.ok) {
      write_error(frame.header.correlation_id, decoded.error, decoded.message, true);
      return;
    }
    if (!storage::is_valid_group_name(request.group)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InvalidGroup,
                  "invalid consumer group name", true);
      return;
    }
    if (!storage::is_valid_topic_name(request.topic)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "invalid topic name", true);
      return;
    }
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    try {
      const auto response = runtime_.reset_group_offset(request);
      const auto payload = protocol::encode_reset_group_offset_response(response);
      write_frame(protocol::FrameType::ResetGroupOffsetResponse, frame.header.correlation_id,
                  payload, false);
    } catch (const BrokerRequestError& error) {
      write_error(frame.header.correlation_id, error.code(), error.what(), false);
    } catch (const std::exception& error) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError, error.what(),
                  false);
    }
  }

  void handle_empty_not_implemented(protocol::Frame frame, std::string_view message) {
    const auto validation = protocol::validate_empty_payload(frame.payload);
    if (!validation.ok) {
      write_error(frame.header.correlation_id, validation.error, validation.message, true);
      return;
    }
    write_error(frame.header.correlation_id, protocol::ErrorCode::NotImplemented, message, false);
  }

  void write_error(std::uint64_t correlation_id, protocol::ErrorCode code, std::string_view message,
                   bool close_after_write) {
    write_structured_log({"warn", "protocol_error", remote_endpoint_,
                          std::string{protocol::frame_type_name(request_frame_type_)},
                          std::string{protocol::error_code_name(code)}, std::string{message},
                          correlation_id, std::nullopt, protocol::is_retryable_error(code),
                          std::nullopt, runtime_.active_long_poll_waiters(),
                          request_duration_ms()});
    auto payload = protocol::encode_error_response(code, message);
    if (runtime_.max_frame_bytes() > protocol::kFrameHeaderBytes &&
        payload.size() > runtime_.max_frame_bytes() - protocol::kFrameHeaderBytes) {
      payload = protocol::encode_error_response(code, protocol::error_code_name(code));
    }
    write_frame(protocol::FrameType::ErrorResponse, correlation_id, payload, close_after_write);
  }

  void write_frame(protocol::FrameType frame_type, std::uint64_t correlation_id,
                   std::span<const std::uint8_t> payload, bool close_after_write) {
    if (frame_type != protocol::FrameType::ErrorResponse) {
      write_structured_log({"info",
                            "protocol_response",
                            remote_endpoint_,
                            std::string{protocol::frame_type_name(frame_type)},
                            {},
                            {},
                            correlation_id,
                            static_cast<std::uint32_t>(payload.size()),
                            std::nullopt,
                            std::nullopt,
                            runtime_.active_long_poll_waiters(),
                            request_duration_ms()});
    }

    auto self = shared_from_this();
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(
        protocol::encode_frame(frame_type, correlation_id, payload));
    boost::asio::async_write(
        socket_, boost::asio::buffer(*bytes),
        [this, self, bytes, close_after_write](const boost::system::error_code& ec, std::size_t) {
          if (ec || !socket_.is_open()) {
            cancel_active_waiter();
            return;
          }
          if (close_after_write) {
            boost::system::error_code ignored;
            socket_.shutdown(Tcp::socket::shutdown_both, ignored);
            socket_.close(ignored);
            return;
          }
          read_header();
        });
  }

  [[nodiscard]] std::optional<std::uint64_t> request_duration_ms() const {
    if (request_started_ == std::chrono::steady_clock::time_point{}) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - request_started_)
                                          .count());
  }

  void cancel_active_waiter() {
    if (!active_waiter_id_) {
      return;
    }
    runtime_.cancel_fetch_waiter(*active_waiter_id_);
    active_waiter_id_.reset();
  }

  Tcp::socket socket_;
  BrokerRuntime& runtime_;
  std::function<bool()> ready_;
  std::function<std::string()> ready_detail_;
  std::function<void()> on_close_;
  std::string remote_endpoint_;
  bool authenticated_{false};
  std::optional<std::uint64_t> active_waiter_id_;
  std::chrono::steady_clock::time_point request_started_{};
  protocol::FrameType request_frame_type_{protocol::FrameType::ErrorResponse};
  std::array<std::uint8_t, protocol::kFrameHeaderBytes> header_buffer_{};
  std::vector<std::uint8_t> payload_buffer_;
};

} // namespace

std::string utc_now_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};

#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif

  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

BrokerServer::BrokerServer(ServerOptions options, BuildInfo build_info)
    : options_(std::move(options)), build_info_(std::move(build_info)),
      startup_time_utc_(utc_now_iso8601()), broker_acceptor_(io_), admin_acceptor_(io_),
      retention_timer_(io_),
      runtime_(std::make_unique<BrokerRuntime>(options_, broker_token_from_environment())) {}

BrokerServer::~BrokerServer() { stop(); }

void BrokerServer::start() {
  prepare_data_directory();

  broker_acceptor_.open(Tcp::v4());
  broker_acceptor_.set_option(Tcp::acceptor::reuse_address(true));
  broker_acceptor_.bind(make_endpoint(options_.listen));
  broker_acceptor_.listen();

  admin_acceptor_.open(Tcp::v4());
  admin_acceptor_.set_option(Tcp::acceptor::reuse_address(true));
  admin_acceptor_.bind(make_endpoint(options_.admin_listen));
  admin_acceptor_.listen();

  stopping_ = false;
  accept_broker_client();
  accept_admin_client();
  schedule_retention();

  write_structured_log({"info",
                        "server_listening",
                        {},
                        {},
                        {},
                        "broker=" + endpoint_to_string(options_.listen) +
                            " admin=" + endpoint_to_string(options_.admin_listen) +
                            " data=" + options_.data_dir.string(),
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        runtime_->active_long_poll_waiters()});
}

void BrokerServer::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  boost::system::error_code ignored;
  broker_acceptor_.close(ignored);
  admin_acceptor_.close(ignored);
  retention_timer_.cancel();
  io_.stop();
}

void BrokerServer::wait_for_shutdown_signal() {
  boost::asio::signal_set signals(io_, SIGINT, SIGTERM);
  signals.async_wait([this](const boost::system::error_code& error, int signal_number) {
    if (!error) {
      write_structured_log({"info",
                            "server_shutdown_signal",
                            {},
                            {},
                            {},
                            "signal=" + std::to_string(signal_number),
                            std::nullopt,
                            std::nullopt,
                            std::nullopt,
                            std::nullopt,
                            runtime_->active_long_poll_waiters()});
      stop();
    }
  });
  io_.run();
}

std::string BrokerServer::version_json() const {
  return build_info_json(build_info_, startup_time_utc_);
}

std::uint16_t BrokerServer::broker_port() const {
  boost::system::error_code ec;
  const auto endpoint = broker_acceptor_.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
}

std::uint16_t BrokerServer::admin_port() const {
  boost::system::error_code ec;
  const auto endpoint = admin_acceptor_.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
}

void BrokerServer::prepare_data_directory() {
  std::error_code ec;
  std::filesystem::create_directories(options_.data_dir, ec);
  if (ec) {
    ready_ = false;
    ready_detail_ = "failed to create data directory: " + ec.message();
    throw std::runtime_error(ready_detail_);
  }

  const auto probe = options_.data_dir / ".boltstream-ready-check";
  {
    std::ofstream out{probe, std::ios::binary | std::ios::trunc};
    if (!out) {
      ready_ = false;
      ready_detail_ = "data directory is not writable";
      throw std::runtime_error(ready_detail_);
    }
    out << "ok\n";
  }
  std::filesystem::remove(probe, ec);

  try {
    const auto summary = runtime_->load_existing_topics();
    const auto retention = runtime_->run_retention({});
    write_structured_log(
        {"info",
         "storage_recovery",
         {},
         {},
         {},
         "topics=" + std::to_string(summary.topics_recovered) +
             " partitions=" + std::to_string(summary.partitions_recovered) +
             " segments=" + std::to_string(summary.segments_scanned) +
             " indexes_rebuilt=" + std::to_string(summary.indexes_rebuilt) +
             " records=" + std::to_string(summary.records_recovered) +
             " bytes_truncated=" + std::to_string(summary.bytes_truncated) +
             " retention_segments_deleted=" + std::to_string(retention.segments_deleted) +
             " retention_bytes_deleted=" + std::to_string(retention.bytes_deleted),
         std::nullopt,
         std::nullopt,
         std::nullopt,
         std::nullopt,
         runtime_->active_long_poll_waiters()});
  } catch (const std::exception& error) {
    ready_ = false;
    ready_detail_ = "storage recovery failed: " + std::string{error.what()};
    throw std::runtime_error(ready_detail_);
  }

  ready_ = true;
  ready_detail_ = "ready";
}

void BrokerServer::schedule_retention() {
  if (options_.retention_check_interval_ms == 0 || stopping_) {
    return;
  }
  retention_timer_.expires_after(std::chrono::milliseconds(options_.retention_check_interval_ms));
  retention_timer_.async_wait([this](const boost::system::error_code& ec) {
    if (ec || stopping_) {
      return;
    }
    try {
      (void)runtime_->run_retention({});
    } catch (const std::exception& error) {
      write_structured_log(
          {"error",
           "retention_failed",
           {},
           {},
           std::string{protocol::error_code_name(protocol::ErrorCode::InternalError)},
           error.what(),
           std::nullopt,
           std::nullopt,
           std::nullopt,
           std::nullopt,
           runtime_->active_long_poll_waiters()});
    }
    schedule_retention();
  });
}

void BrokerServer::accept_broker_client() {
  auto socket = std::make_shared<Tcp::socket>(io_);
  broker_acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
    if (ec) {
      if (broker_acceptor_.is_open() && ec != boost::asio::error::operation_aborted) {
        write_structured_log({"error",
                              "broker_accept_error",
                              {},
                              {},
                              {},
                              ec.message(),
                              std::nullopt,
                              std::nullopt,
                              std::nullopt,
                              std::nullopt,
                              runtime_->active_long_poll_waiters()});
      }
      return;
    }
    handle_broker_client(std::move(*socket));
    if (broker_acceptor_.is_open()) {
      accept_broker_client();
    }
  });
}

void BrokerServer::accept_admin_client() {
  auto socket = std::make_shared<Tcp::socket>(io_);
  admin_acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
    if (ec) {
      if (admin_acceptor_.is_open() && ec != boost::asio::error::operation_aborted) {
        write_structured_log({"error",
                              "admin_accept_error",
                              {},
                              {},
                              {},
                              ec.message(),
                              std::nullopt,
                              std::nullopt,
                              std::nullopt,
                              std::nullopt,
                              runtime_->active_long_poll_waiters()});
      }
      return;
    }
    handle_admin_client(std::move(*socket));
    if (admin_acceptor_.is_open()) {
      accept_admin_client();
    }
  });
}

void BrokerServer::handle_broker_client(Tcp::socket socket) {
  const auto active = active_broker_sessions_.load();
  if (active >= options_.max_broker_connections) {
    boost::system::error_code remote_ec;
    const auto remote = socket.remote_endpoint(remote_ec);
    write_structured_log({"warn",
                          "broker_connection_rejected",
                          remote_ec ? std::string{} : endpoint_string(remote),
                          {},
                          std::string{protocol::error_code_name(protocol::ErrorCode::Overloaded)},
                          "broker connection limit reached",
                          std::nullopt,
                          std::nullopt,
                          protocol::is_retryable_error(protocol::ErrorCode::Overloaded),
                          std::nullopt,
                          runtime_->active_long_poll_waiters()});
    boost::system::error_code ignored;
    socket.shutdown(Tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    return;
  }

  active_broker_sessions_.fetch_add(1);
  std::make_shared<BrokerProtocolSession>(
      std::move(socket), *runtime_, [this] { return ready(); }, [this] { return ready_detail_; },
      [this] { active_broker_sessions_.fetch_sub(1); })
      ->start();
}

void BrokerServer::handle_admin_client(Tcp::socket socket) {
  auto client = std::make_shared<Tcp::socket>(std::move(socket));
  auto buffer = std::make_shared<std::array<char, 2048>>();
  client->async_read_some(
      boost::asio::buffer(*buffer),
      [this, client, buffer](const boost::system::error_code& ec, std::size_t read) {
        if (ec && ec != boost::asio::error::eof) {
          return;
        }

        const auto request = std::string_view{buffer->data(), read};
        const auto path = normalize_request_path(request);
        auto response = std::make_shared<std::string>();

        if (path == "/health/live") {
          *response = http_response("200 OK", "application/json", health_json("live"));
        } else if (path == "/health/ready") {
          *response =
              http_response(ready_ ? "200 OK" : "503 Service Unavailable", "application/json",
                            health_json(ready_ ? "ready" : "not_ready"));
        } else if (path == "/version") {
          *response = http_response("200 OK", "application/json", version_json());
        } else {
          *response = http_response("404 Not Found", "application/json",
                                    "{\"status\":\"not_found\",\"service\":\"boltstream\"}");
        }

        boost::asio::async_write(*client, boost::asio::buffer(*response),
                                 [client, response](const boost::system::error_code&, std::size_t) {
                                   boost::system::error_code ignored;
                                   client->shutdown(Tcp::socket::shutdown_both, ignored);
                                 });
      });
}

BrokerServer::Tcp::endpoint BrokerServer::make_endpoint(const Endpoint& endpoint) const {
  boost::system::error_code ec;
  const auto address = boost::asio::ip::make_address(endpoint.host, ec);
  if (ec) {
    throw std::runtime_error("invalid endpoint host " + endpoint.host + ": " + ec.message());
  }
  return {address, endpoint.port};
}

std::string BrokerServer::health_json(std::string_view status) const {
  std::ostringstream out;
  out << "{";
  out << "\"service\":\"boltstream\",";
  out << "\"status\":\"" << status << "\",";
  out << "\"git_sha\":\"" << build_info_.git_sha << "\",";
  out << "\"detail\":\"" << ready_detail_ << "\"";
  out << "}";
  return out.str();
}

std::string BrokerServer::http_response(std::string_view status, std::string_view content_type,
                                        std::string_view body) const {
  std::ostringstream out;
  out << "HTTP/1.1 " << status << "\r\n";
  out << "Content-Type: " << content_type << "\r\n";
  out << "Content-Length: " << body.size() << "\r\n";
  out << "Connection: close\r\n";
  out << "\r\n";
  out << body;
  return out.str();
}

} // namespace boltstream::broker
