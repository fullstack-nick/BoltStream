#include "boltstream/broker/server.h"

#include "boltstream/protocol/protocol.h"
#include "boltstream/storage/partition_log.h"

#include <boost/asio/read.hpp>
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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace boltstream::broker {

class BrokerRuntime {
  struct TopicState {
    explicit TopicState(storage::PartitionLog opened_log) : log(std::move(opened_log)) {}

    mutable std::mutex mutex;
    storage::PartitionLog log;
  };

public:
  explicit BrokerRuntime(ServerOptions options, std::string broker_token)
      : data_dir_(std::move(options.data_dir)), max_frame_bytes_(options.max_frame_bytes),
        max_fetch_records_(options.max_fetch_records), max_fetch_bytes_(options.max_fetch_bytes),
        broker_token_(std::move(broker_token)) {}

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

      const auto partition_dir =
          storage::partition_directory(data_dir_, topic, storage::kPhaseThreePartition);
      if (!std::filesystem::exists(partition_dir, ec)) {
        continue;
      }

      auto log = storage::PartitionLog::open(
          {data_dir_, topic, storage::kPhaseThreePartition, storage::kDefaultMaxSegmentBytes});
      const auto& stats = log.recovery_stats();
      topics_[topic] = std::make_shared<TopicState>(std::move(log));

      ++summary.topics_recovered;
      ++summary.partitions_recovered;
      summary.segments_scanned += stats.segments_scanned;
      summary.indexes_rebuilt += stats.indexes_rebuilt;
      summary.records_recovered += stats.records_recovered;
      summary.bytes_truncated += stats.bytes_truncated;
    }
    return summary;
  }

  [[nodiscard]] bool auth_required() const { return !broker_token_.empty(); }

  [[nodiscard]] bool authenticate(std::string_view token) const {
    return !auth_required() || token == broker_token_;
  }

  protocol::ProduceResponse produce(const protocol::ProduceRequest& request) {
    auto topic = open_or_create_topic(request.topic);
    std::lock_guard log_lock{topic->mutex};
    const auto metadata = topic->log.append(request.key, request.message);

    protocol::ProduceResponse response;
    response.topic = metadata.topic;
    response.partition = metadata.partition;
    response.offset = metadata.offset;
    response.next_offset = topic->log.next_offset();
    response.encoded_byte_size = metadata.encoded_byte_size;
    return response;
  }

  [[nodiscard]] std::uint64_t next_offset_or_zero(std::string_view topic_name) const {
    const auto topic = find_topic(topic_name);
    if (!topic) {
      return 0;
    }
    std::lock_guard log_lock{topic->mutex};
    return topic->log.next_offset();
  }

  protocol::FetchResponse fetch(std::string_view topic_name, std::uint64_t from_offset) const {
    protocol::FetchResponse response;
    response.topic = std::string{topic_name};
    response.partition = storage::kPhaseThreePartition;
    response.from_offset = from_offset;

    const auto topic = find_topic(topic_name);
    if (!topic) {
      response.next_offset = 0;
      return response;
    }

    std::lock_guard log_lock{topic->mutex};
    const auto records = topic->log.read_from(from_offset, max_fetch_records_, max_fetch_bytes_);
    response.next_offset = topic->log.next_offset();
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

  protocol::MetadataResponse metadata() const {
    protocol::MetadataResponse response;
    std::lock_guard topics_lock{topics_mutex_};
    response.topics.reserve(topics_.size());
    for (const auto& [topic_name, topic] : topics_) {
      std::lock_guard log_lock{topic->mutex};
      response.topics.push_back(
          {topic_name, storage::kPhaseThreePartition, topic->log.next_offset()});
    }
    return response;
  }

  [[nodiscard]] std::uint32_t max_frame_bytes() const { return max_frame_bytes_; }

private:
  std::shared_ptr<TopicState> open_or_create_topic(std::string_view topic_name) {
    std::lock_guard lock{topics_mutex_};
    const auto name = std::string{topic_name};
    const auto existing = topics_.find(name);
    if (existing != topics_.end()) {
      return existing->second;
    }

    auto log = storage::PartitionLog::open(
        {data_dir_, name, storage::kPhaseThreePartition, storage::kDefaultMaxSegmentBytes});
    auto topic = std::make_shared<TopicState>(std::move(log));
    topics_.emplace(name, topic);
    return topic;
  }

  [[nodiscard]] std::shared_ptr<TopicState> find_topic(std::string_view topic_name) const {
    std::lock_guard lock{topics_mutex_};
    const auto existing = topics_.find(std::string{topic_name});
    return existing == topics_.end() ? nullptr : existing->second;
  }

  std::filesystem::path data_dir_;
  std::uint32_t max_frame_bytes_;
  std::uint32_t max_fetch_records_;
  std::uint32_t max_fetch_bytes_;
  std::string broker_token_;
  mutable std::mutex topics_mutex_;
  std::map<std::string, std::shared_ptr<TopicState>> topics_;
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

bool parse_u64(std::string_view text, std::uint64_t& value) {
  if (text.empty()) {
    return false;
  }
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto parsed = std::from_chars(begin, end, value);
  return parsed.ec == std::errc{} && parsed.ptr == end;
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
      handle_empty_not_implemented(std::move(frame),
                                   "offset commits are implemented with consumer groups");
      return;
    case protocol::FrameType::AuthRequest:
      handle_auth(std::move(frame));
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
    if (!require_auth(frame.header.correlation_id)) {
      return;
    }

    std::uint64_t from_offset = 0;
    if (request.from == "beginning") {
      from_offset = 0;
    } else if (request.from == "latest") {
      from_offset = runtime_.next_offset_or_zero(request.topic);
    } else if (!parse_u64(request.from, from_offset)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::MalformedPayload,
                  "fetch offset selector must be beginning, latest, or an unsigned offset", true);
      return;
    }

    try {
      auto response = runtime_.fetch(request.topic, from_offset);
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
        write_error(frame.header.correlation_id, protocol::ErrorCode::InternalError,
                    "fetch response exceeds configured maximum frame size", false);
        return;
      }

      write_frame(protocol::FrameType::FetchResponse, frame.header.correlation_id, payload, false);
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
