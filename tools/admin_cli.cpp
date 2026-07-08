#include "boltstream/client/async_client.h"

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct AdminOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9000};
  int timeout_ms{5000};
  std::string command;
  std::string subcommand;
  std::string topic;
  std::string group;
  std::string to;
  std::uint16_t partitions{0};
  std::uint16_t partition{0};
  bool has_partition{false};
  std::string token;
  bool help_requested{false};
  std::string error;
};

void usage() {
  std::cout << "Usage:\n"
               "  boltstream-admin topics create --topic TOPIC --partitions N [common]\n"
               "  boltstream-admin topics list [common]\n"
               "  boltstream-admin topics describe --topic TOPIC [common]\n"
               "  boltstream-admin topics delete --topic TOPIC [common]\n"
               "  boltstream-admin retention run [--topic TOPIC] [common]\n"
               "  boltstream-admin groups describe --group GROUP --topic TOPIC [common]\n"
               "  boltstream-admin groups reset-offset --group GROUP --topic TOPIC --partition N "
               "--to TARGET [common]\n"
               "\n"
               "Common options: [--host HOST] [--port PORT] [--timeout-ms MS] [--token TOKEN]\n"
               "TARGET is beginning, latest, or an unsigned offset.\n"
               "If --token is omitted, BOLTSTREAM_BROKER_TOKEN is used when present.\n";
}

bool parse_u16(std::string_view text, std::uint16_t& value, bool allow_zero = false) {
  unsigned long parsed = 0;
  try {
    const auto as_string = std::string{text};
    std::size_t consumed = 0;
    parsed = std::stoul(as_string, &consumed);
    if (consumed != as_string.size() || (!allow_zero && parsed == 0) || parsed > 65535) {
      return false;
    }
  } catch (...) {
    return false;
  }
  value = static_cast<std::uint16_t>(parsed);
  return true;
}

bool parse_int(std::string_view text, int& value) {
  try {
    const auto as_string = std::string{text};
    std::size_t consumed = 0;
    const auto parsed = std::stoi(as_string, &consumed);
    if (consumed != as_string.size() || parsed <= 0) {
      return false;
    }
    value = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<std::string> environment_value(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t value_size = 0;
  if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string out{value};
  std::free(value);
  return out;
#else
  const auto* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string{value};
#endif
}

bool is_command(const AdminOptions& options, std::string_view command,
                std::string_view subcommand) {
  return options.command == command && options.subcommand == subcommand;
}

AdminOptions parse_options(int argc, char** argv) {
  AdminOptions options;
  if (argc >= 2) {
    options.command = argv[1];
  }
  if (argc >= 3) {
    options.subcommand = argv[2];
  }

  if (argc <= 1 || options.command == "--help" || options.command == "-h") {
    options.help_requested = true;
    return options;
  }

  const auto known_command =
      is_command(options, "topics", "create") || is_command(options, "topics", "list") ||
      is_command(options, "topics", "describe") || is_command(options, "topics", "delete") ||
      is_command(options, "retention", "run") || is_command(options, "groups", "describe") ||
      is_command(options, "groups", "reset-offset");
  if (!known_command) {
    options.error = "unknown command";
    return options;
  }

  for (int index = 3; index < argc; ++index) {
    const std::string_view arg{argv[index]};
    auto require_value = [&](std::string_view name) -> std::string_view {
      if (index + 1 >= argc) {
        options.error = "missing value for " + std::string{name};
        return {};
      }
      ++index;
      return argv[index];
    };

    if (arg == "--help" || arg == "-h") {
      options.help_requested = true;
    } else if (arg == "--host") {
      options.host = std::string{require_value(arg)};
    } else if (arg == "--port") {
      if (!parse_u16(require_value(arg), options.port)) {
        options.error = "invalid --port value";
        return options;
      }
    } else if (arg == "--timeout-ms") {
      if (!parse_int(require_value(arg), options.timeout_ms)) {
        options.error = "invalid --timeout-ms value";
        return options;
      }
    } else if (arg == "--topic") {
      options.topic = std::string{require_value(arg)};
    } else if (arg == "--partitions") {
      if (!parse_u16(require_value(arg), options.partitions)) {
        options.error = "invalid --partitions value";
        return options;
      }
    } else if (arg == "--group") {
      options.group = std::string{require_value(arg)};
    } else if (arg == "--partition") {
      if (!parse_u16(require_value(arg), options.partition, true)) {
        options.error = "invalid --partition value";
        return options;
      }
      options.has_partition = true;
    } else if (arg == "--to") {
      options.to = std::string{require_value(arg)};
    } else if (arg == "--token") {
      options.token = std::string{require_value(arg)};
    } else {
      options.error = "unknown argument: " + std::string{arg};
      return options;
    }
  }

  if (options.help_requested) {
    return options;
  }
  if (options.token.empty()) {
    if (auto token = environment_value("BOLTSTREAM_BROKER_TOKEN")) {
      options.token = *token;
    }
  }

  if (is_command(options, "topics", "create")) {
    if (options.topic.empty()) {
      options.error = "--topic is required";
    } else if (options.partitions == 0) {
      options.error = "--partitions is required";
    }
  } else if (is_command(options, "topics", "describe") || is_command(options, "topics", "delete")) {
    if (options.topic.empty()) {
      options.error = "--topic is required";
    }
  } else if (is_command(options, "groups", "describe")) {
    if (options.group.empty() || options.topic.empty()) {
      options.error = "--group and --topic are required";
    }
  } else if (is_command(options, "groups", "reset-offset")) {
    if (options.group.empty() || options.topic.empty() || !options.has_partition ||
        options.to.empty()) {
      options.error = "--group, --topic, --partition, and --to are required";
    }
  }
  return options;
}

std::string json_escape(std::string_view value) {
  std::string out;
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  return out;
}

void print_partition_description(const boltstream::protocol::TopicPartitionDescription& partition) {
  std::cout << "{\"partition\":" << partition.partition
            << ",\"earliest_offset\":" << partition.earliest_offset
            << ",\"next_offset\":" << partition.next_offset
            << ",\"segment_count\":" << partition.segment_count
            << ",\"log_bytes\":" << partition.log_bytes << "}";
}

void print_topic_description(const boltstream::protocol::TopicDescription& topic) {
  std::cout << "{\"topic\":\"" << json_escape(topic.topic)
            << "\",\"partition_count\":" << topic.partition_count
            << ",\"log_bytes\":" << topic.log_bytes << ",\"partitions\":[";
  for (std::size_t index = 0; index < topic.partitions.size(); ++index) {
    if (index > 0) {
      std::cout << ",";
    }
    print_partition_description(topic.partitions[index]);
  }
  std::cout << "]}";
}

int print_error_response(const boltstream::protocol::Frame& frame) {
  boltstream::protocol::ErrorResponse response;
  const auto decoded = boltstream::protocol::decode_error_response(frame.payload, response);
  if (!decoded.ok) {
    std::cerr << "boltstream-admin: malformed error response: " << decoded.message << '\n';
    return 1;
  }
  const auto retryable = boltstream::protocol::is_retryable_error(response.code);
  std::cout << "{\"status\":\"" << boltstream::protocol::error_code_name(response.code)
            << "\",\"error_code\":\"" << boltstream::protocol::error_code_name(response.code)
            << "\",\"retryable\":" << (retryable ? "true" : "false") << ",\"message\":\""
            << json_escape(response.message)
            << "\",\"correlation_id\":" << frame.header.correlation_id << "}\n";
  return retryable ? 5 : 1;
}

int response_exit_code(const boltstream::protocol::Frame& frame) {
  using boltstream::protocol::FrameType;
  if (frame.header.frame_type == FrameType::ErrorResponse) {
    return print_error_response(frame);
  }

  switch (frame.header.frame_type) {
  case FrameType::CreateTopicResponse: {
    boltstream::protocol::CreateTopicResponse response;
    const auto decoded =
        boltstream::protocol::decode_create_topic_response(frame.payload, response);
    if (!decoded.ok) {
      std::cerr << "boltstream-admin: malformed create-topic response: " << decoded.message << '\n';
      return 1;
    }
    std::cout << "{\"status\":\"" << json_escape(response.status) << "\",\"topic\":\""
              << json_escape(response.topic) << "\",\"partitions\":" << response.partition_count
              << ",\"correlation_id\":" << frame.header.correlation_id << "}\n";
    return 0;
  }
  case FrameType::ListTopicsResponse: {
    boltstream::protocol::ListTopicsResponse response;
    const auto decoded = boltstream::protocol::decode_list_topics_response(frame.payload, response);
    if (!decoded.ok) {
      std::cerr << "boltstream-admin: malformed list-topics response: " << decoded.message << '\n';
      return 1;
    }
    std::cout << "{\"status\":\"ok\",\"topic_count\":" << response.topics.size() << ",\"topics\":[";
    for (std::size_t index = 0; index < response.topics.size(); ++index) {
      if (index > 0) {
        std::cout << ",";
      }
      print_topic_description(response.topics[index]);
    }
    std::cout << "],\"correlation_id\":" << frame.header.correlation_id << "}\n";
    return 0;
  }
  case FrameType::DescribeTopicResponse: {
    boltstream::protocol::DescribeTopicResponse response;
    const auto decoded =
        boltstream::protocol::decode_describe_topic_response(frame.payload, response);
    if (!decoded.ok) {
      std::cerr << "boltstream-admin: malformed describe-topic response: " << decoded.message
                << '\n';
      return 1;
    }
    std::cout << "{\"status\":\"ok\",\"topic\":";
    print_topic_description(response.topic);
    std::cout << ",\"correlation_id\":" << frame.header.correlation_id << "}\n";
    return 0;
  }
  case FrameType::DeleteTopicResponse: {
    boltstream::protocol::DeleteTopicResponse response;
    const auto decoded =
        boltstream::protocol::decode_delete_topic_response(frame.payload, response);
    if (!decoded.ok) {
      std::cerr << "boltstream-admin: malformed delete-topic response: " << decoded.message << '\n';
      return 1;
    }
    std::cout << "{\"status\":\"" << json_escape(response.status) << "\",\"topic\":\""
              << json_escape(response.topic)
              << "\",\"partitions_deleted\":" << response.partitions_deleted
              << ",\"segments_deleted\":" << response.segments_deleted
              << ",\"bytes_deleted\":" << response.bytes_deleted
              << ",\"offsets_removed\":" << response.offsets_removed
              << ",\"correlation_id\":" << frame.header.correlation_id << "}\n";
    return 0;
  }
  case FrameType::RunRetentionResponse: {
    boltstream::protocol::RunRetentionResponse response;
    const auto decoded =
        boltstream::protocol::decode_run_retention_response(frame.payload, response);
    if (!decoded.ok) {
      std::cerr << "boltstream-admin: malformed run-retention response: " << decoded.message
                << '\n';
      return 1;
    }
    std::cout << "{\"status\":\"ok\",\"topic\":\"" << json_escape(response.topic)
              << "\",\"topics_scanned\":" << response.topics_scanned
              << ",\"partitions_scanned\":" << response.partitions_scanned
              << ",\"segments_deleted\":" << response.segments_deleted
              << ",\"bytes_deleted\":" << response.bytes_deleted << ",\"partitions\":[";
    for (std::size_t index = 0; index < response.partitions.size(); ++index) {
      const auto& partition = response.partitions[index];
      if (index > 0) {
        std::cout << ",";
      }
      std::cout << "{\"topic\":\"" << json_escape(partition.topic)
                << "\",\"partition\":" << partition.partition
                << ",\"segments_deleted\":" << partition.segments_deleted
                << ",\"bytes_deleted\":" << partition.bytes_deleted
                << ",\"earliest_offset\":" << partition.earliest_offset
                << ",\"next_offset\":" << partition.next_offset << "}";
    }
    std::cout << "],\"correlation_id\":" << frame.header.correlation_id << "}\n";
    return 0;
  }
  case FrameType::DescribeGroupResponse: {
    boltstream::protocol::DescribeGroupResponse response;
    const auto decoded =
        boltstream::protocol::decode_describe_group_response(frame.payload, response);
    if (!decoded.ok) {
      std::cerr << "boltstream-admin: malformed describe-group response: " << decoded.message
                << '\n';
      return 1;
    }
    std::cout << "{\"status\":\"ok\",\"group\":\"" << json_escape(response.group)
              << "\",\"topic\":\"" << json_escape(response.topic)
              << "\",\"active_member_count\":" << response.active_member_count << ",\"offsets\":[";
    for (std::size_t index = 0; index < response.offsets.size(); ++index) {
      const auto& offset = response.offsets[index];
      if (index > 0) {
        std::cout << ",";
      }
      std::cout << "{\"partition\":" << offset.partition
                << ",\"has_committed_offset\":" << (offset.has_committed_offset ? "true" : "false")
                << ",\"committed_offset\":" << offset.committed_offset
                << ",\"earliest_offset\":" << offset.earliest_offset
                << ",\"next_offset\":" << offset.next_offset << ",\"lag\":" << offset.lag
                << ",\"out_of_range\":" << (offset.out_of_range ? "true" : "false") << "}";
    }
    std::cout << "],\"correlation_id\":" << frame.header.correlation_id << "}\n";
    return 0;
  }
  case FrameType::ResetGroupOffsetResponse: {
    boltstream::protocol::ResetGroupOffsetResponse response;
    const auto decoded =
        boltstream::protocol::decode_reset_group_offset_response(frame.payload, response);
    if (!decoded.ok) {
      std::cerr << "boltstream-admin: malformed reset-group-offset response: " << decoded.message
                << '\n';
      return 1;
    }
    std::cout << "{\"status\":\"" << json_escape(response.status) << "\",\"group\":\""
              << json_escape(response.group) << "\",\"topic\":\"" << json_escape(response.topic)
              << "\",\"partition\":" << response.partition
              << ",\"next_offset\":" << response.next_offset
              << ",\"correlation_id\":" << frame.header.correlation_id << "}\n";
    return 0;
  }
  default:
    std::cerr << "boltstream-admin: unexpected response frame type "
              << boltstream::protocol::frame_type_name(frame.header.frame_type) << '\n';
    return 1;
  }
}

bool auth_response_ok(const boltstream::protocol::Frame& frame) {
  if (frame.header.frame_type != boltstream::protocol::FrameType::AuthResponse) {
    return false;
  }
  boltstream::protocol::AuthResponse response;
  const auto decoded = boltstream::protocol::decode_auth_response(frame.payload, response);
  return decoded.ok && (response.status == "authenticated" || response.status == "disabled");
}

std::pair<boltstream::protocol::FrameType, std::vector<std::uint8_t>>
build_request(const AdminOptions& options) {
  using boltstream::protocol::FrameType;
  if (is_command(options, "topics", "create")) {
    return {FrameType::CreateTopicRequest,
            boltstream::protocol::encode_create_topic_request(options.topic, options.partitions)};
  }
  if (is_command(options, "topics", "list")) {
    return {FrameType::ListTopicsRequest, {}};
  }
  if (is_command(options, "topics", "describe")) {
    return {FrameType::DescribeTopicRequest,
            boltstream::protocol::encode_describe_topic_request({options.topic})};
  }
  if (is_command(options, "topics", "delete")) {
    return {FrameType::DeleteTopicRequest,
            boltstream::protocol::encode_delete_topic_request({options.topic})};
  }
  if (is_command(options, "retention", "run")) {
    return {FrameType::RunRetentionRequest,
            boltstream::protocol::encode_run_retention_request({options.topic})};
  }
  if (is_command(options, "groups", "describe")) {
    return {FrameType::DescribeGroupRequest,
            boltstream::protocol::encode_describe_group_request({options.group, options.topic})};
  }
  return {FrameType::ResetGroupOffsetRequest,
          boltstream::protocol::encode_reset_group_offset_request(
              {options.group, options.topic, options.partition, options.to})};
}

} // namespace

int main(int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (options.help_requested) {
    usage();
    return 0;
  }
  if (!options.error.empty()) {
    std::cerr << "boltstream-admin: " << options.error << "\n\n";
    usage();
    return 2;
  }

  auto [frame_type, payload] = build_request(options);
  boost::asio::io_context io;
  auto client = std::make_shared<boltstream::client::AsyncClient>(io);
  boost::asio::steady_timer timer{io};
  int exit_code = 1;
  bool finished = false;

  auto finish = [&](int code) {
    if (finished) {
      return;
    }
    finished = true;
    exit_code = code;
    timer.cancel();
    client->close();
  };

  timer.expires_after(std::chrono::milliseconds(options.timeout_ms));
  timer.async_wait([&](const boost::system::error_code& ec) {
    if (!ec) {
      std::cerr << "boltstream-admin: timed out waiting for broker response\n";
      finish(4);
    }
  });

  client->async_connect(
      options.host, options.port,
      [&, client, frame_type,
       payload = std::move(payload)](const boost::system::error_code& ec) mutable {
        if (ec) {
          std::cerr << "boltstream-admin: connect failed: " << ec.message() << '\n';
          finish(1);
          return;
        }

        auto send_request = [&, client, frame_type, payload = std::move(payload)]() mutable {
          client->async_request(
              frame_type, std::move(payload),
              [&](const boost::system::error_code& request_ec, boltstream::protocol::Frame frame) {
                if (request_ec) {
                  std::cerr << "boltstream-admin: request failed: " << request_ec.message() << '\n';
                  finish(1);
                  return;
                }
                finish(response_exit_code(frame));
              });
        };

        if (options.token.empty()) {
          send_request();
          return;
        }

        client->async_auth(options.token, [&, send_request = std::move(send_request)](
                                              const boost::system::error_code& auth_ec,
                                              boltstream::protocol::Frame frame) mutable {
          if (auth_ec) {
            std::cerr << "boltstream-admin: auth failed: " << auth_ec.message() << '\n';
            finish(1);
            return;
          }
          if (frame.header.frame_type == boltstream::protocol::FrameType::ErrorResponse) {
            finish(response_exit_code(frame));
            return;
          }
          if (!auth_response_ok(frame)) {
            std::cerr << "boltstream-admin: malformed auth response\n";
            finish(1);
            return;
          }
          send_request();
        });
      });
  io.run();
  return exit_code;
}
