#include "boltstream/client/async_client.h"

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct ConsumerOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9000};
  int timeout_ms{5000};
  std::string topic;
  std::string from;
  std::string group;
  std::uint16_t partition{0};
  std::uint32_t wait_ms{0};
  std::uint32_t session_timeout_ms{3000};
  std::uint32_t heartbeat_ms{1000};
  std::uint32_t poll_ms{200};
  std::uint32_t max_records{0};
  std::uint32_t idle_exit_ms{0};
  std::string token;
  boltstream::compression::Codec compression{boltstream::compression::Codec::None};
  bool help_requested{false};
  bool from_seen{false};
  bool partition_seen{false};
  bool wait_seen{false};
  bool commit{false};
  bool coordinated{false};
  std::string error;
};

void usage() {
  std::cout << "Usage: boltstream-consumer --topic TOPIC [--partition N] "
               "[--from beginning|latest|committed|OFFSET] [--group GROUP]\n"
               "                           [--host HOST] [--port PORT] [--timeout-ms MS]\n"
               "                           [--wait-ms MS] [--commit] [--token TOKEN]\n"
               "                           [--compression none|zstd]\n"
               "       boltstream-consumer --coordinated --topic TOPIC --group GROUP --commit\n"
               "                           [--session-timeout-ms MS] [--heartbeat-ms MS]\n"
               "                           [--poll-ms MS] [--max-records N] [--idle-exit-ms MS]\n"
               "\n"
               "Sends a binary protocol FetchRequest and prints returned records as JSON.\n"
               "If --group is set and --from is omitted, --from defaults to committed.\n"
               "Coordinated mode joins a broker-managed group and prints JSON Lines events.\n"
               "If --token is omitted, BOLTSTREAM_BROKER_TOKEN is used when present.\n";
}

bool parse_u16(std::string_view text, std::uint16_t& value) {
  unsigned long parsed = 0;
  try {
    const auto as_string = std::string{text};
    std::size_t consumed = 0;
    parsed = std::stoul(as_string, &consumed);
    if (consumed != as_string.size() || parsed == 0 || parsed > 65535) {
      return false;
    }
  } catch (...) {
    return false;
  }
  value = static_cast<std::uint16_t>(parsed);
  return true;
}

bool parse_partition(std::string_view text, std::uint16_t& value) {
  unsigned long parsed = 0;
  try {
    const auto as_string = std::string{text};
    std::size_t consumed = 0;
    parsed = std::stoul(as_string, &consumed);
    if (consumed != as_string.size() || parsed > 65535) {
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

ConsumerOptions parse_options(int argc, char** argv) {
  ConsumerOptions options;
  for (int index = 1; index < argc; ++index) {
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
    } else if (arg == "--from") {
      options.from = std::string{require_value(arg)};
      options.from_seen = true;
    } else if (arg == "--group") {
      options.group = std::string{require_value(arg)};
    } else if (arg == "--compression") {
      const auto value = require_value(arg);
      if (value == "none") {
        options.compression = boltstream::compression::Codec::None;
      } else if (value == "zstd") {
        options.compression = boltstream::compression::Codec::Zstd;
      } else {
        options.error = "--compression must be none or zstd";
        return options;
      }
    } else if (arg == "--partition") {
      if (!parse_partition(require_value(arg), options.partition)) {
        options.error = "invalid --partition value";
        return options;
      }
      options.partition_seen = true;
    } else if (arg == "--wait-ms") {
      int wait_ms = 0;
      if (!parse_int(require_value(arg), wait_ms)) {
        options.error = "invalid --wait-ms value";
        return options;
      }
      options.wait_ms = static_cast<std::uint32_t>(wait_ms);
      options.wait_seen = true;
    } else if (arg == "--commit") {
      options.commit = true;
    } else if (arg == "--coordinated") {
      options.coordinated = true;
    } else if (arg == "--session-timeout-ms") {
      int value = 0;
      if (!parse_int(require_value(arg), value)) {
        options.error = "invalid --session-timeout-ms value";
        return options;
      }
      options.session_timeout_ms = static_cast<std::uint32_t>(value);
    } else if (arg == "--heartbeat-ms") {
      int value = 0;
      if (!parse_int(require_value(arg), value)) {
        options.error = "invalid --heartbeat-ms value";
        return options;
      }
      options.heartbeat_ms = static_cast<std::uint32_t>(value);
    } else if (arg == "--poll-ms") {
      int value = 0;
      if (!parse_int(require_value(arg), value)) {
        options.error = "invalid --poll-ms value";
        return options;
      }
      options.poll_ms = static_cast<std::uint32_t>(value);
    } else if (arg == "--max-records") {
      int value = 0;
      if (!parse_int(require_value(arg), value)) {
        options.error = "invalid --max-records value";
        return options;
      }
      options.max_records = static_cast<std::uint32_t>(value);
    } else if (arg == "--idle-exit-ms") {
      int value = 0;
      if (!parse_int(require_value(arg), value)) {
        options.error = "invalid --idle-exit-ms value";
        return options;
      }
      options.idle_exit_ms = static_cast<std::uint32_t>(value);
    } else if (arg == "--token") {
      options.token = std::string{require_value(arg)};
    } else {
      options.error = "unknown argument: " + std::string{arg};
      return options;
    }
  }

  if (!options.help_requested) {
    if (options.token.empty()) {
      if (auto token = environment_value("BOLTSTREAM_BROKER_TOKEN")) {
        options.token = *token;
      }
    }
    if (options.topic.empty()) {
      options.error = "--topic is required";
    } else if (options.commit && options.group.empty()) {
      options.error = "--commit requires --group";
    } else if (options.coordinated && options.group.empty()) {
      options.error = "--coordinated requires --group";
    } else if (options.coordinated && !options.commit) {
      options.error = "--coordinated requires --commit";
    } else if (options.coordinated && options.partition_seen) {
      options.error = "--coordinated assigns partitions automatically and rejects --partition";
    } else if (options.coordinated && options.wait_seen) {
      options.error = "--coordinated uses client polling and rejects --wait-ms";
    } else if (options.coordinated && options.from_seen) {
      options.error = "--coordinated always fetches from committed offsets and rejects --from";
    } else if (options.coordinated && options.heartbeat_ms >= options.session_timeout_ms) {
      options.error = "--heartbeat-ms must be less than --session-timeout-ms";
    } else if (options.from.empty()) {
      options.from = options.group.empty() ? "beginning" : "committed";
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

std::string as_string(const std::vector<std::uint8_t>& bytes) {
  return {bytes.begin(), bytes.end()};
}

int response_exit_code(const boltstream::protocol::Frame& frame,
                       std::optional<std::uint64_t> committed_offset = std::nullopt) {
  if (frame.header.frame_type == boltstream::protocol::FrameType::FetchResponse) {
    boltstream::protocol::FetchResponse response;
    const auto decoded = boltstream::protocol::decode_fetch_response(frame.payload, response);
    if (!decoded.ok) {
      std::cerr << "boltstream-consumer: malformed fetch response: " << decoded.message << '\n';
      return 1;
    }

    std::cout << "{\"status\":\"ok\",\"topic\":\"" << json_escape(response.topic)
              << "\",\"partition\":" << response.partition << ",\"from\":" << response.from_offset
              << ",\"count\":" << response.records.size()
              << ",\"next_offset\":" << response.next_offset;
    if (committed_offset) {
      std::cout << ",\"committed_offset\":" << *committed_offset;
    }
    std::cout << ",\"records\":[";
    for (std::size_t index = 0; index < response.records.size(); ++index) {
      const auto& record = response.records[index];
      if (index > 0) {
        std::cout << ",";
      }
      std::cout << "{\"offset\":" << record.offset
                << ",\"timestamp_unix_ns\":" << record.timestamp_unix_ns << ",\"key\":\""
                << json_escape(as_string(record.key)) << "\",\"message\":\""
                << json_escape(as_string(record.message))
                << "\",\"encoded_bytes\":" << record.encoded_byte_size << "}";
    }
    std::cout << "],\"correlation_id\":" << frame.header.correlation_id << "}\n";
    return 0;
  }

  if (frame.header.frame_type != boltstream::protocol::FrameType::ErrorResponse) {
    std::cerr << "boltstream-consumer: unexpected response frame type "
              << boltstream::protocol::frame_type_name(frame.header.frame_type) << '\n';
    return 1;
  }

  boltstream::protocol::ErrorResponse response;
  const auto decoded = boltstream::protocol::decode_error_response(frame.payload, response);
  if (!decoded.ok) {
    std::cerr << "boltstream-consumer: malformed error response: " << decoded.message << '\n';
    return 1;
  }

  const auto retryable = boltstream::protocol::is_retryable_error(response.code);
  std::cout << "{\"status\":\"" << boltstream::protocol::error_code_name(response.code)
            << "\",\"error_code\":\"" << boltstream::protocol::error_code_name(response.code)
            << "\",\"retryable\":" << (retryable ? "true" : "false") << ",\"message\":\""
            << json_escape(response.message)
            << "\",\"correlation_id\":" << frame.header.correlation_id << "}\n";
  if (retryable) {
    return 5;
  }
  return response.code == boltstream::protocol::ErrorCode::NotImplemented ? 3 : 1;
}

bool decode_fetch_success(const boltstream::protocol::Frame& frame,
                          boltstream::protocol::FetchResponse& response) {
  if (frame.header.frame_type != boltstream::protocol::FrameType::FetchResponse) {
    return false;
  }
  const auto decoded = boltstream::protocol::decode_fetch_response(frame.payload, response);
  if (!decoded.ok) {
    std::cerr << "boltstream-consumer: malformed fetch response: " << decoded.message << '\n';
    return false;
  }
  return true;
}

bool auth_response_ok(const boltstream::protocol::Frame& frame) {
  if (frame.header.frame_type != boltstream::protocol::FrameType::AuthResponse) {
    return false;
  }
  boltstream::protocol::AuthResponse response;
  const auto decoded = boltstream::protocol::decode_auth_response(frame.payload, response);
  return decoded.ok && (response.status == "authenticated" || response.status == "disabled");
}

struct RequestResult {
  bool ok{false};
  bool timed_out{false};
  std::string error;
  boltstream::protocol::Frame frame;
};

RequestResult request_once(const ConsumerOptions& options,
                           boltstream::protocol::FrameType frame_type,
                           std::vector<std::uint8_t> payload) {
  boost::asio::io_context io;
  auto client = std::make_shared<boltstream::client::AsyncClient>(io);
  boost::asio::steady_timer timer{io};
  RequestResult result;
  bool finished = false;

  auto finish_error = [&](std::string message, bool timed_out = false) {
    if (finished) {
      return;
    }
    finished = true;
    result.ok = false;
    result.timed_out = timed_out;
    result.error = std::move(message);
    timer.cancel();
    client->close();
    io.stop();
  };

  auto finish_frame = [&](boltstream::protocol::Frame frame) {
    if (finished) {
      return;
    }
    finished = true;
    result.ok = true;
    result.frame = std::move(frame);
    timer.cancel();
    client->close();
    io.stop();
  };

  auto target_payload = std::make_shared<std::vector<std::uint8_t>>(std::move(payload));
  auto send_target = [&, client, target_payload]() mutable {
    client->async_request(
        frame_type, std::move(*target_payload),
        [&](const boost::system::error_code& request_ec, boltstream::protocol::Frame frame) {
          if (request_ec) {
            finish_error("request failed: " + request_ec.message());
            return;
          }
          finish_frame(std::move(frame));
        });
  };
  auto negotiate_then_target = [&, client, send_target]() mutable {
    const auto codecs = boltstream::compression::kNoneMask |
                        (options.compression == boltstream::compression::Codec::Zstd
                             ? boltstream::compression::kZstdMask
                             : 0U);
    client->async_metadata(
        [&, send_target](const boost::system::error_code& metadata_ec,
                         boltstream::protocol::Frame frame) mutable {
          if (metadata_ec ||
              frame.header.frame_type != boltstream::protocol::FrameType::MetadataResponse) {
            finish_frame(std::move(frame));
            return;
          }
          send_target();
        },
        codecs);
  };

  timer.expires_after(std::chrono::milliseconds(options.timeout_ms));
  timer.async_wait([&](const boost::system::error_code& ec) {
    if (!ec) {
      finish_error("timed out waiting for broker response", true);
    }
  });

  client->async_connect(
      options.host, options.port, [&, client](const boost::system::error_code& ec) {
        if (ec) {
          finish_error("connect failed: " + ec.message());
          return;
        }

        if (options.token.empty()) {
          negotiate_then_target();
          return;
        }

        client->async_auth(
            options.token, [&, negotiate_then_target](const boost::system::error_code& auth_ec,
                                                      boltstream::protocol::Frame frame) mutable {
              if (auth_ec) {
                finish_error("auth failed: " + auth_ec.message());
                return;
              }
              if (frame.header.frame_type == boltstream::protocol::FrameType::ErrorResponse) {
                finish_frame(std::move(frame));
                return;
              }
              if (!auth_response_ok(frame)) {
                finish_error("malformed auth response");
                return;
              }
              negotiate_then_target();
            });
      });

  io.run();
  return result;
}

std::optional<boltstream::protocol::ErrorResponse>
decode_error_frame(const boltstream::protocol::Frame& frame) {
  if (frame.header.frame_type != boltstream::protocol::FrameType::ErrorResponse) {
    return std::nullopt;
  }
  boltstream::protocol::ErrorResponse response;
  const auto decoded = boltstream::protocol::decode_error_response(frame.payload, response);
  if (!decoded.ok) {
    return std::nullopt;
  }
  return response;
}

bool should_rejoin(const boltstream::protocol::Frame& frame) {
  const auto error = decode_error_frame(frame);
  if (!error) {
    return false;
  }
  return error->code == boltstream::protocol::ErrorCode::RebalanceRequired ||
         error->code == boltstream::protocol::ErrorCode::StaleMember;
}

void print_rebalance_event(const boltstream::protocol::Frame& frame, std::string_view member_id,
                           std::uint64_t generation_id) {
  const auto error = decode_error_frame(frame);
  std::cout << "{\"event\":\"rebalance\",\"member_id\":\"" << json_escape(member_id)
            << "\",\"generation_id\":" << generation_id;
  if (error) {
    std::cout << ",\"error_code\":\"" << boltstream::protocol::error_code_name(error->code)
              << "\",\"retryable\":"
              << (boltstream::protocol::is_retryable_error(error->code) ? "true" : "false")
              << ",\"message\":\"" << json_escape(error->message) << "\"";
  }
  std::cout << "}\n";
}

void print_assignment_event(std::string_view member_id, std::uint64_t generation_id,
                            const std::vector<std::uint16_t>& assignment) {
  std::cout << "{\"event\":\"assignment\",\"member_id\":\"" << json_escape(member_id)
            << "\",\"generation_id\":" << generation_id << ",\"partitions\":[";
  for (std::size_t index = 0; index < assignment.size(); ++index) {
    if (index > 0) {
      std::cout << ",";
    }
    std::cout << assignment[index];
  }
  std::cout << "]}\n";
}

void print_record_event(std::string_view member_id, std::uint64_t generation_id,
                        const boltstream::protocol::FetchResponse& response,
                        const boltstream::protocol::FetchRecord& record) {
  std::cout << "{\"event\":\"record\",\"member_id\":\"" << json_escape(member_id)
            << "\",\"generation_id\":" << generation_id << ",\"topic\":\""
            << json_escape(response.topic) << "\",\"partition\":" << response.partition
            << ",\"offset\":" << record.offset << ",\"key\":\""
            << json_escape(as_string(record.key)) << "\",\"message\":\""
            << json_escape(as_string(record.message)) << "\"}\n";
}

int transport_failure_exit(const RequestResult& result, std::string_view operation) {
  std::cerr << "boltstream-consumer: " << operation << " " << result.error << '\n';
  return result.timed_out ? 4 : 1;
}

int run_coordinated_consumer(const ConsumerOptions& options) {
  std::cout << std::unitbuf;
  std::string member_id;
  std::uint64_t generation_id = 0;
  std::vector<std::uint16_t> assignment;
  std::uint32_t records_seen = 0;
  auto last_activity = std::chrono::steady_clock::now();
  auto last_heartbeat = std::chrono::steady_clock::now();

  auto join_and_sync = [&]() -> int {
    for (int attempt = 0; attempt < 8; ++attempt) {
      boltstream::protocol::JoinGroupRequest join_request;
      join_request.group = options.group;
      join_request.topic = options.topic;
      join_request.member_id = member_id;
      join_request.session_timeout_ms = options.session_timeout_ms;
      auto join_result =
          request_once(options, boltstream::protocol::FrameType::JoinGroupRequest,
                       boltstream::protocol::encode_join_group_request(join_request));
      if (!join_result.ok) {
        return transport_failure_exit(join_result, "join-group");
      }
      if (join_result.frame.header.frame_type == boltstream::protocol::FrameType::ErrorResponse) {
        return response_exit_code(join_result.frame);
      }
      if (join_result.frame.header.frame_type !=
          boltstream::protocol::FrameType::JoinGroupResponse) {
        std::cerr << "boltstream-consumer: unexpected join response frame type "
                  << boltstream::protocol::frame_type_name(join_result.frame.header.frame_type)
                  << '\n';
        return 1;
      }
      boltstream::protocol::JoinGroupResponse join_response;
      auto decoded = boltstream::protocol::decode_join_group_response(join_result.frame.payload,
                                                                      join_response);
      if (!decoded.ok) {
        std::cerr << "boltstream-consumer: malformed join response: " << decoded.message << '\n';
        return 1;
      }
      member_id = join_response.member_id;
      generation_id = join_response.generation_id;
      std::cout << "{\"event\":\"join\",\"member_id\":\"" << json_escape(member_id)
                << "\",\"generation_id\":" << generation_id << ",\"group\":\""
                << json_escape(options.group) << "\",\"topic\":\"" << json_escape(options.topic)
                << "\"}\n";

      boltstream::protocol::SyncGroupRequest sync_request;
      sync_request.group = options.group;
      sync_request.topic = options.topic;
      sync_request.member_id = member_id;
      sync_request.generation_id = generation_id;
      auto sync_result =
          request_once(options, boltstream::protocol::FrameType::SyncGroupRequest,
                       boltstream::protocol::encode_sync_group_request(sync_request));
      if (!sync_result.ok) {
        return transport_failure_exit(sync_result, "sync-group");
      }
      if (sync_result.frame.header.frame_type == boltstream::protocol::FrameType::ErrorResponse) {
        if (should_rejoin(sync_result.frame)) {
          print_rebalance_event(sync_result.frame, member_id, generation_id);
          continue;
        }
        return response_exit_code(sync_result.frame);
      }
      if (sync_result.frame.header.frame_type !=
          boltstream::protocol::FrameType::SyncGroupResponse) {
        std::cerr << "boltstream-consumer: unexpected sync response frame type "
                  << boltstream::protocol::frame_type_name(sync_result.frame.header.frame_type)
                  << '\n';
        return 1;
      }
      boltstream::protocol::SyncGroupResponse sync_response;
      decoded = boltstream::protocol::decode_sync_group_response(sync_result.frame.payload,
                                                                 sync_response);
      if (!decoded.ok) {
        std::cerr << "boltstream-consumer: malformed sync response: " << decoded.message << '\n';
        return 1;
      }
      assignment = std::move(sync_response.assignment);
      print_assignment_event(member_id, generation_id, assignment);
      last_heartbeat = std::chrono::steady_clock::now();
      return 0;
    }
    std::cerr << "boltstream-consumer: too many rebalances while joining group\n";
    return 5;
  };

  auto join_code = join_and_sync();
  if (join_code != 0) {
    return join_code;
  }

  bool running = true;
  while (running) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat >= std::chrono::milliseconds(options.heartbeat_ms)) {
      boltstream::protocol::HeartbeatRequest heartbeat_request;
      heartbeat_request.group = options.group;
      heartbeat_request.topic = options.topic;
      heartbeat_request.member_id = member_id;
      heartbeat_request.generation_id = generation_id;
      auto heartbeat_result =
          request_once(options, boltstream::protocol::FrameType::HeartbeatRequest,
                       boltstream::protocol::encode_heartbeat_request(heartbeat_request));
      if (!heartbeat_result.ok) {
        return transport_failure_exit(heartbeat_result, "heartbeat");
      }
      if (heartbeat_result.frame.header.frame_type ==
          boltstream::protocol::FrameType::ErrorResponse) {
        if (should_rejoin(heartbeat_result.frame)) {
          print_rebalance_event(heartbeat_result.frame, member_id, generation_id);
          const auto code = join_and_sync();
          if (code != 0) {
            return code;
          }
          continue;
        }
        return response_exit_code(heartbeat_result.frame);
      }
      last_heartbeat = std::chrono::steady_clock::now();
    }

    bool saw_records = false;
    for (const auto partition : assignment) {
      auto fetch_result =
          request_once(options, boltstream::protocol::FrameType::FetchRequest,
                       boltstream::protocol::encode_fetch_request(options.topic, partition,
                                                                  "committed", options.group, 0));
      if (!fetch_result.ok) {
        return transport_failure_exit(fetch_result, "fetch");
      }
      if (fetch_result.frame.header.frame_type == boltstream::protocol::FrameType::ErrorResponse) {
        return response_exit_code(fetch_result.frame);
      }
      boltstream::protocol::FetchResponse fetch_response;
      const auto decoded =
          boltstream::protocol::decode_fetch_response(fetch_result.frame.payload, fetch_response);
      if (!decoded.ok) {
        std::cerr << "boltstream-consumer: malformed fetch response: " << decoded.message << '\n';
        return 1;
      }
      if (fetch_response.records.empty()) {
        continue;
      }

      saw_records = true;
      std::uint64_t commit_next_offset = fetch_response.next_offset;
      for (const auto& record : fetch_response.records) {
        print_record_event(member_id, generation_id, fetch_response, record);
        ++records_seen;
        commit_next_offset = record.offset + 1;
        if (options.max_records != 0 && records_seen >= options.max_records) {
          running = false;
          break;
        }
      }

      boltstream::protocol::GroupOffsetCommitRequest commit_request;
      commit_request.group = options.group;
      commit_request.topic = options.topic;
      commit_request.member_id = member_id;
      commit_request.generation_id = generation_id;
      commit_request.partition = partition;
      commit_request.next_offset = commit_next_offset;
      auto commit_result =
          request_once(options, boltstream::protocol::FrameType::GroupOffsetCommitRequest,
                       boltstream::protocol::encode_group_offset_commit_request(commit_request));
      if (!commit_result.ok) {
        return transport_failure_exit(commit_result, "group-offset-commit");
      }
      if (commit_result.frame.header.frame_type == boltstream::protocol::FrameType::ErrorResponse) {
        if (should_rejoin(commit_result.frame)) {
          print_rebalance_event(commit_result.frame, member_id, generation_id);
          const auto code = join_and_sync();
          if (code != 0) {
            return code;
          }
          break;
        }
        return response_exit_code(commit_result.frame);
      }
      boltstream::protocol::GroupOffsetCommitResponse commit_response;
      const auto decoded_commit = boltstream::protocol::decode_group_offset_commit_response(
          commit_result.frame.payload, commit_response);
      if (!decoded_commit.ok) {
        std::cerr << "boltstream-consumer: malformed group commit response: "
                  << decoded_commit.message << '\n';
        return 1;
      }
      std::cout << "{\"event\":\"commit\",\"member_id\":\"" << json_escape(member_id)
                << "\",\"generation_id\":" << generation_id << ",\"topic\":\""
                << json_escape(commit_response.topic)
                << "\",\"partition\":" << commit_response.partition
                << ",\"next_offset\":" << commit_response.next_offset << "}\n";

      if (!running) {
        break;
      }
    }

    if (saw_records) {
      last_activity = std::chrono::steady_clock::now();
    }
    if (!running) {
      break;
    }
    if (options.idle_exit_ms != 0 && std::chrono::steady_clock::now() - last_activity >=
                                         std::chrono::milliseconds(options.idle_exit_ms)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
  }

  boltstream::protocol::LeaveGroupRequest leave_request;
  leave_request.group = options.group;
  leave_request.topic = options.topic;
  leave_request.member_id = member_id;
  leave_request.generation_id = generation_id;
  auto leave_result = request_once(options, boltstream::protocol::FrameType::LeaveGroupRequest,
                                   boltstream::protocol::encode_leave_group_request(leave_request));
  if (leave_result.ok &&
      leave_result.frame.header.frame_type == boltstream::protocol::FrameType::LeaveGroupResponse) {
    boltstream::protocol::LeaveGroupResponse leave_response;
    const auto decoded = boltstream::protocol::decode_leave_group_response(
        leave_result.frame.payload, leave_response);
    if (decoded.ok) {
      std::cout << "{\"event\":\"leave\",\"member_id\":\"" << json_escape(member_id)
                << "\",\"generation_id\":" << leave_response.generation_id << ",\"status\":\""
                << json_escape(leave_response.status) << "\"}\n";
    }
  }

  std::cout << "{\"event\":\"summary\",\"member_id\":\"" << json_escape(member_id)
            << "\",\"generation_id\":" << generation_id << ",\"records\":" << records_seen << "}\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (options.help_requested) {
    usage();
    return 0;
  }
  if (!options.error.empty()) {
    std::cerr << "boltstream-consumer: " << options.error << "\n\n";
    usage();
    return 2;
  }
  if (options.coordinated) {
    return run_coordinated_consumer(options);
  }

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
      std::cerr << "boltstream-consumer: timed out waiting for broker response\n";
      finish(4);
    }
  });

  client->async_connect(
      options.host, options.port, [&, client](const boost::system::error_code& ec) {
        if (ec) {
          std::cerr << "boltstream-consumer: connect failed: " << ec.message() << '\n';
          finish(1);
          return;
        }

        auto send_fetch = [&, client] {
          client->async_fetch(
              options.topic, options.partition, options.from, options.group, options.wait_ms,
              [&, client](const boost::system::error_code& request_ec,
                          boltstream::protocol::Frame frame) {
                if (request_ec) {
                  std::cerr << "boltstream-consumer: request failed: " << request_ec.message()
                            << '\n';
                  finish(1);
                  return;
                }
                if (!options.commit) {
                  finish(response_exit_code(frame));
                  return;
                }

                boltstream::protocol::FetchResponse fetch_response;
                if (!decode_fetch_success(frame, fetch_response)) {
                  finish(response_exit_code(frame));
                  return;
                }

                boltstream::protocol::OffsetCommitRequest commit_request;
                commit_request.group = options.group;
                commit_request.topic = fetch_response.topic;
                commit_request.partition = fetch_response.partition;
                commit_request.next_offset = fetch_response.next_offset;

                client->async_offset_commit(
                    commit_request, [&, client, fetch_frame = std::move(frame)](
                                        const boost::system::error_code& commit_ec,
                                        boltstream::protocol::Frame commit_frame) mutable {
                      if (commit_ec) {
                        std::cerr << "boltstream-consumer: commit failed: " << commit_ec.message()
                                  << '\n';
                        finish(1);
                        return;
                      }
                      if (commit_frame.header.frame_type ==
                          boltstream::protocol::FrameType::ErrorResponse) {
                        finish(response_exit_code(commit_frame));
                        return;
                      }
                      if (commit_frame.header.frame_type !=
                          boltstream::protocol::FrameType::OffsetCommitResponse) {
                        std::cerr << "boltstream-consumer: unexpected commit response frame type "
                                  << boltstream::protocol::frame_type_name(
                                         commit_frame.header.frame_type)
                                  << '\n';
                        finish(1);
                        return;
                      }

                      boltstream::protocol::OffsetCommitResponse commit_response;
                      const auto decoded = boltstream::protocol::decode_offset_commit_response(
                          commit_frame.payload, commit_response);
                      if (!decoded.ok) {
                        std::cerr << "boltstream-consumer: malformed commit response: "
                                  << decoded.message << '\n';
                        finish(1);
                        return;
                      }
                      finish(response_exit_code(fetch_frame, commit_response.next_offset));
                    });
              });
        };
        auto negotiate_then_fetch = [&, client, send_fetch]() mutable {
          const auto codecs = boltstream::compression::kNoneMask |
                              (options.compression == boltstream::compression::Codec::Zstd
                                   ? boltstream::compression::kZstdMask
                                   : 0U);
          client->async_metadata(
              [&, send_fetch](const boost::system::error_code& metadata_ec,
                              boltstream::protocol::Frame frame) mutable {
                if (metadata_ec ||
                    frame.header.frame_type != boltstream::protocol::FrameType::MetadataResponse) {
                  finish(response_exit_code(frame));
                  return;
                }
                send_fetch();
              },
              codecs);
        };

        if (options.token.empty()) {
          negotiate_then_fetch();
          return;
        }

        client->async_auth(
            options.token, [&, negotiate_then_fetch](const boost::system::error_code& auth_ec,
                                                     boltstream::protocol::Frame frame) mutable {
              if (auth_ec) {
                std::cerr << "boltstream-consumer: auth failed: " << auth_ec.message() << '\n';
                finish(1);
                return;
              }
              if (frame.header.frame_type == boltstream::protocol::FrameType::ErrorResponse) {
                finish(response_exit_code(frame));
                return;
              }
              if (!auth_response_ok(frame)) {
                std::cerr << "boltstream-consumer: malformed auth response\n";
                finish(1);
                return;
              }
              negotiate_then_fetch();
            });
      });
  io.run();
  return exit_code;
}
