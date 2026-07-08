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
  std::string token;
  bool help_requested{false};
  bool from_seen{false};
  bool commit{false};
  std::string error;
};

void usage() {
  std::cout << "Usage: boltstream-consumer --topic TOPIC [--partition N] "
               "[--from beginning|latest|committed|OFFSET] [--group GROUP]\n"
               "                           [--host HOST] [--port PORT] [--timeout-ms MS]\n"
               "                           [--wait-ms MS] [--commit] [--token TOKEN]\n"
               "\n"
               "Sends a binary protocol FetchRequest and prints returned records as JSON.\n"
               "If --group is set and --from is omitted, --from defaults to committed.\n"
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
    } else if (arg == "--partition") {
      if (!parse_partition(require_value(arg), options.partition)) {
        options.error = "invalid --partition value";
        return options;
      }
    } else if (arg == "--wait-ms") {
      int wait_ms = 0;
      if (!parse_int(require_value(arg), wait_ms)) {
        options.error = "invalid --wait-ms value";
        return options;
      }
      options.wait_ms = static_cast<std::uint32_t>(wait_ms);
    } else if (arg == "--commit") {
      options.commit = true;
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

        if (options.token.empty()) {
          send_fetch();
          return;
        }

        client->async_auth(
            options.token, [&, send_fetch](const boost::system::error_code& auth_ec,
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
              send_fetch();
            });
      });
  io.run();
  return exit_code;
}
