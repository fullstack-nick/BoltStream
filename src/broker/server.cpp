#include "boltstream/broker/server.h"

#include "boltstream/protocol/protocol.h"
#include "boltstream/storage/offset_store.h"
#include "boltstream/storage/partition_log.h"

#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
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
  struct TopicState {
    struct PartitionState {
      explicit PartitionState(storage::PartitionLog opened_log) : log(std::move(opened_log)) {}

      mutable std::mutex mutex;
      storage::PartitionLog log;
    };

    mutable std::mutex mutex;
    std::string name;
    std::uint16_t partition_count{0};
    std::uint16_t next_round_robin_partition{0};
    std::vector<std::shared_ptr<PartitionState>> partitions;
  };

public:
  explicit BrokerRuntime(ServerOptions options, std::string broker_token)
      : data_dir_(std::move(options.data_dir)), max_frame_bytes_(options.max_frame_bytes),
        max_fetch_records_(options.max_fetch_records), max_fetch_bytes_(options.max_fetch_bytes),
        max_topic_partitions_(options.max_topic_partitions),
        max_fetch_wait_ms_(options.max_fetch_wait_ms), broker_token_(std::move(broker_token)),
        offset_store_(storage::OffsetStore::open(data_dir_)) {}

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

  protocol::ProduceResponse produce(const protocol::ProduceRequest& request) {
    auto topic = find_topic_or_throw(request.topic);
    const auto partition_id = select_partition(*topic, request.key);
    auto partition = topic->partitions.at(partition_id);

    protocol::ProduceResponse response;
    {
      std::lock_guard log_lock{partition->mutex};
      const auto metadata = partition->log.append(request.key, request.message);
      response.topic = metadata.topic;
      response.partition = metadata.partition;
      response.offset = metadata.offset;
      response.next_offset = partition->log.next_offset();
      response.encoded_byte_size = metadata.encoded_byte_size;
    }
    notify_fetch_waiters(response.topic, response.partition);
    return response;
  }

  protocol::FetchResponse fetch(const protocol::FetchRequest& request) const {
    auto topic = find_topic_or_throw(request.topic);
    auto partition = partition_or_throw(*topic, request.partition);
    std::lock_guard log_lock{partition->mutex};
    const auto next_offset = partition->log.next_offset();
    const auto from_offset = resolve_fetch_offset(request, next_offset);

    protocol::FetchResponse response;
    response.topic = request.topic;
    response.partition = request.partition;
    response.from_offset = from_offset;
    response.next_offset = next_offset;
    const auto records =
        partition->log.read_from(from_offset, max_fetch_records_, max_fetch_bytes_);
    response.records.reserve(records.size());
    for (const auto& record : records) {
      protocol::FetchRecord out;
      out.offset = record.metadata.offset;
      out.timestamp_unix_ns = record.metadata.timestamp_unix_ns;
      out.key = record.key;
      out.message = record.value;
      out.encoded_byte_size = record.metadata.encoded_byte_size;
      response.records.push_back(std::move(out));
    }
    return response;
  }

  protocol::OffsetCommitResponse commit_offset(const protocol::OffsetCommitRequest& request) {
    validate_group_or_throw(request.group);
    auto topic = find_topic_or_throw(request.topic);
    auto partition = partition_or_throw(*topic, request.partition);
    std::lock_guard log_lock{partition->mutex};
    if (request.next_offset > partition->log.next_offset()) {
      throw BrokerRequestError{protocol::ErrorCode::InvalidOffset,
                               "commit offset is beyond partition next offset"};
    }

    std::lock_guard offset_lock{offsets_mutex_};
    offset_store_.commit(request.group, request.topic, request.partition, request.next_offset);
    return {request.group, request.topic, request.partition, request.next_offset};
  }

  protocol::MetadataResponse metadata() const {
    protocol::MetadataResponse response;
    std::lock_guard topics_lock{topics_mutex_};
    for (const auto& [topic_name, topic] : topics_) {
      response.topics.reserve(response.topics.size() + topic->partitions.size());
      for (const auto& partition : topic->partitions) {
        std::lock_guard log_lock{partition->mutex};
        response.topics.push_back(
            {topic_name, partition->log.options().partition_id, partition->log.next_offset()});
      }
    }
    return response;
  }

  [[nodiscard]] std::uint32_t max_frame_bytes() const { return max_frame_bytes_; }
  [[nodiscard]] std::uint32_t max_fetch_wait_ms() const { return max_fetch_wait_ms_; }

  std::uint64_t register_fetch_waiter(std::string topic, std::uint16_t partition,
                                      std::function<void()> callback) {
    std::lock_guard lock{waiters_mutex_};
    const auto id = next_waiter_id_++;
    waiters_[{std::move(topic), partition}].push_back({id, std::move(callback)});
    return id;
  }

  void cancel_fetch_waiter(std::uint64_t waiter_id) {
    std::lock_guard lock{waiters_mutex_};
    for (auto it = waiters_.begin(); it != waiters_.end();) {
      auto& callbacks = it->second;
      callbacks.erase(
          std::remove_if(callbacks.begin(), callbacks.end(),
                         [waiter_id](const auto& waiter) { return waiter.id == waiter_id; }),
          callbacks.end());
      if (callbacks.empty()) {
        it = waiters_.erase(it);
      } else {
        ++it;
      }
    }
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

  std::shared_ptr<TopicState> open_topic_state(std::string_view topic_name,
                                               std::uint16_t partition_count) const {
    auto topic = std::make_shared<TopicState>();
    topic->name = std::string{topic_name};
    topic->partition_count = partition_count;
    topic->partitions.reserve(partition_count);
    for (std::uint16_t partition = 0; partition < partition_count; ++partition) {
      auto log = storage::PartitionLog::open(
          {data_dir_, topic->name, partition, storage::kDefaultMaxSegmentBytes});
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
                                                   std::uint64_t next_offset) const {
    if (request.from == "beginning") {
      return 0;
    }
    if (request.from == "latest") {
      return next_offset;
    }
    if (request.from == "committed") {
      validate_group_or_throw(request.group);
      std::lock_guard offset_lock{offsets_mutex_};
      return offset_store_.committed(request.group, request.topic, request.partition).value_or(0);
    }

    std::uint64_t from_offset = 0;
    if (!parse_u64(request.from, from_offset)) {
      throw BrokerRequestError{
          protocol::ErrorCode::MalformedPayload,
          "fetch offset selector must be beginning, latest, committed, or an unsigned offset"};
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
      waiters_.erase(existing);
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

  std::filesystem::path data_dir_;
  std::uint32_t max_frame_bytes_;
  std::uint32_t max_fetch_records_;
  std::uint32_t max_fetch_bytes_;
  std::uint32_t max_topic_partitions_;
  std::uint32_t max_fetch_wait_ms_;
  std::string broker_token_;
  mutable std::mutex topics_mutex_;
  std::map<std::string, std::shared_ptr<TopicState>> topics_;
  mutable std::mutex offsets_mutex_;
  storage::OffsetStore offset_store_;
  mutable std::mutex waiters_mutex_;
  std::uint64_t next_waiter_id_{1};
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
                        std::function<std::string()> ready_detail)
      : socket_(std::move(socket)), runtime_(runtime), ready_(std::move(ready)),
        ready_detail_(std::move(ready_detail)), authenticated_(!runtime_.auth_required()) {}

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
    std::cerr << "protocol request correlation_id=" << frame.header.correlation_id
              << " type=" << protocol::frame_type_name(frame.header.frame_type)
              << " payload_bytes=" << frame.header.payload_bytes << '\n';

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
      const auto response = runtime_.produce(request);
      const auto payload = protocol::encode_produce_response(response);
      write_frame(protocol::FrameType::ProduceResponse, frame.header.correlation_id, payload,
                  false);
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
    auto self = shared_from_this();
    auto completed = std::make_shared<std::atomic_bool>(false);
    auto timer = std::make_shared<boost::asio::steady_timer>(socket_.get_executor());
    auto waiter_id = std::make_shared<std::uint64_t>(0);
    const auto wait_ms = request.max_wait_ms;
    request.max_wait_ms = 0;
    request.from = std::to_string(from_offset);

    *waiter_id = runtime_.register_fetch_waiter(
        request.topic, request.partition,
        [this, self, completed, timer, correlation_id, request]() mutable {
          if (completed->exchange(true)) {
            return;
          }
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

    timer->expires_after(std::chrono::milliseconds(wait_ms));
    timer->async_wait([this, self, completed, correlation_id, request,
                       waiter_id](const boost::system::error_code& ec) mutable {
      if (ec || completed->exchange(true)) {
        return;
      }
      runtime_.cancel_fetch_waiter(*waiter_id);
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
    while (payload.size() > max_payload_bytes && response.records.size() > 1) {
      response.records.pop_back();
      payload = protocol::encode_fetch_response(response);
    }
    if (payload.size() > max_payload_bytes) {
      write_error(correlation_id, protocol::ErrorCode::InternalError,
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
    const auto payload = protocol::encode_error_response(code, message);
    write_frame(protocol::FrameType::ErrorResponse, correlation_id, payload, close_after_write);
  }

  void write_frame(protocol::FrameType frame_type, std::uint64_t correlation_id,
                   std::span<const std::uint8_t> payload, bool close_after_write) {
    auto self = shared_from_this();
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(
        protocol::encode_frame(frame_type, correlation_id, payload));
    boost::asio::async_write(
        socket_, boost::asio::buffer(*bytes),
        [this, self, bytes, close_after_write](const boost::system::error_code& ec, std::size_t) {
          if (ec || !socket_.is_open()) {
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

  Tcp::socket socket_;
  BrokerRuntime& runtime_;
  std::function<bool()> ready_;
  std::function<std::string()> ready_detail_;
  bool authenticated_{false};
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

  std::cerr << "boltstream-server listening on " << endpoint_to_string(options_.listen)
            << " admin=" << endpoint_to_string(options_.admin_listen)
            << " data=" << options_.data_dir.string() << '\n';
}

void BrokerServer::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  boost::system::error_code ignored;
  broker_acceptor_.close(ignored);
  admin_acceptor_.close(ignored);
  io_.stop();
}

void BrokerServer::wait_for_shutdown_signal() {
  boost::asio::signal_set signals(io_, SIGINT, SIGTERM);
  signals.async_wait([this](const boost::system::error_code& error, int signal_number) {
    if (!error) {
      std::cerr << "boltstream-server shutting down on signal " << signal_number << '\n';
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
    std::cerr << "storage recovery topics=" << summary.topics_recovered
              << " partitions=" << summary.partitions_recovered
              << " segments=" << summary.segments_scanned
              << " indexes_rebuilt=" << summary.indexes_rebuilt
              << " records=" << summary.records_recovered
              << " bytes_truncated=" << summary.bytes_truncated << '\n';
  } catch (const std::exception& error) {
    ready_ = false;
    ready_detail_ = "storage recovery failed: " + std::string{error.what()};
    throw std::runtime_error(ready_detail_);
  }

  ready_ = true;
  ready_detail_ = "ready";
}

void BrokerServer::accept_broker_client() {
  auto socket = std::make_shared<Tcp::socket>(io_);
  broker_acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
    if (ec) {
      if (broker_acceptor_.is_open() && ec != boost::asio::error::operation_aborted) {
        std::cerr << "broker accept error: " << ec.message() << '\n';
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
        std::cerr << "admin accept error: " << ec.message() << '\n';
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
  std::make_shared<BrokerProtocolSession>(
      std::move(socket), *runtime_, [this] { return ready(); }, [this] { return ready_detail_; })
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
