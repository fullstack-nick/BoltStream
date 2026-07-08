#include "boltstream/client/async_client.h"

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ProducerOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9000};
  int timeout_ms{5000};
  std::string topic;
  std::string key;
  std::string message;
  bool help_requested{false};
  std::string error;
};

void usage() {
  std::cout << "Usage: boltstream-producer --topic TOPIC --message VALUE [--key KEY]\n"
               "                           [--host HOST] [--port PORT] [--timeout-ms MS]\n"
               "\n"
               "Phase 2 sends a real binary protocol ProduceRequest. The broker returns a "
               "structured not_implemented response until durable produce/fetch support lands.\n";
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

ProducerOptions parse_options(int argc, char** argv) {
  ProducerOptions options;
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
    } else if (arg == "--key") {
      options.key = std::string{require_value(arg)};
    } else if (arg == "--message") {
      options.message = std::string{require_value(arg)};
    } else {
      options.error = "unknown argument: " + std::string{arg};
      return options;
    }
  }

  if (!options.help_requested) {
    if (options.topic.empty()) {
      options.error = "--topic is required";
    } else if (options.message.empty()) {
      options.error = "--message is required";
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

int response_exit_code(const boltstream::protocol::Frame& frame) {
  if (frame.header.frame_type != boltstream::protocol::FrameType::ErrorResponse) {
    std::cout << "{\"status\":\"ok\",\"correlation_id\":" << frame.header.correlation_id << "}\n";
    return 0;
  }

  boltstream::protocol::ErrorResponse response;
  const auto decoded = boltstream::protocol::decode_error_response(frame.payload, response);
  if (!decoded.ok) {
    std::cerr << "boltstream-producer: malformed error response: " << decoded.message << '\n';
    return 1;
  }

  std::cout << "{\"status\":\"" << boltstream::protocol::error_code_name(response.code)
            << "\",\"error_code\":\"" << boltstream::protocol::error_code_name(response.code)
            << "\",\"message\":\"" << json_escape(response.message)
            << "\",\"correlation_id\":" << frame.header.correlation_id << "}\n";
  return response.code == boltstream::protocol::ErrorCode::NotImplemented ? 3 : 1;
}

} // namespace

int main(int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (options.help_requested) {
    usage();
    return 0;
  }
  if (!options.error.empty()) {
    std::cerr << "boltstream-producer: " << options.error << "\n\n";
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
      std::cerr << "boltstream-producer: timed out waiting for broker response\n";
      finish(4);
    }
  });

  const std::vector<std::uint8_t> key_bytes{options.key.begin(), options.key.end()};
  const std::vector<std::uint8_t> message_bytes{options.message.begin(), options.message.end()};
  client->async_connect(
      options.host, options.port, [&, client](const boost::system::error_code& ec) {
        if (ec) {
          std::cerr << "boltstream-producer: connect failed: " << ec.message() << '\n';
          finish(1);
          return;
        }
        client->async_produce(
            options.topic, key_bytes, message_bytes,
            [&](const boost::system::error_code& request_ec, boltstream::protocol::Frame frame) {
              if (request_ec) {
                std::cerr << "boltstream-producer: request failed: " << request_ec.message()
                          << '\n';
                finish(1);
                return;
              }
              finish(response_exit_code(frame));
            });
      });
  io.run();
  return exit_code;
}
