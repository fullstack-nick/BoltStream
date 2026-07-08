#include "boltstream/broker/options.h"

#include <charconv>
#include <limits>
#include <sstream>

namespace boltstream::broker {
namespace {

bool parse_port(std::string_view text, std::uint16_t& port) {
  unsigned int parsed{};
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  if (parsed == 0 || parsed > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  port = static_cast<std::uint16_t>(parsed);
  return true;
}

bool parse_u32(std::string_view text, std::uint32_t& value, bool allow_zero = false) {
  std::uint32_t parsed{};
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || (!allow_zero && parsed == 0)) {
    return false;
  }
  value = parsed;
  return true;
}

bool parse_endpoint(std::string_view text, Endpoint& endpoint) {
  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return false;
  }

  std::uint16_t port{};
  if (!parse_port(text.substr(colon + 1), port)) {
    return false;
  }

  endpoint.host = std::string{text.substr(0, colon)};
  endpoint.port = port;
  return true;
}

} // namespace

ParsedServerOptions parse_server_options(std::span<const std::string_view> args) {
  ParsedServerOptions parsed;

  for (std::size_t index = 0; index < args.size(); ++index) {
    const auto arg = args[index];

    auto require_value = [&](std::string_view name) -> std::string_view {
      if (index + 1 >= args.size()) {
        parsed.error = "missing value for " + std::string{name};
        return {};
      }
      ++index;
      return args[index];
    };

    if (arg == "--help" || arg == "-h") {
      parsed.help_requested = true;
    } else if (arg == "--version") {
      parsed.version_requested = true;
    } else if (arg == "--listen") {
      Endpoint endpoint;
      if (!parse_endpoint(require_value(arg), endpoint)) {
        parsed.error = "invalid --listen endpoint, expected host:port";
        return parsed;
      }
      parsed.options.listen = endpoint;
    } else if (arg == "--admin-listen") {
      Endpoint endpoint;
      if (!parse_endpoint(require_value(arg), endpoint)) {
        parsed.error = "invalid --admin-listen endpoint, expected host:port";
        return parsed;
      }
      parsed.options.admin_listen = endpoint;
    } else if (arg == "--port") {
      std::uint16_t port{};
      if (!parse_port(require_value(arg), port)) {
        parsed.error = "invalid --port value";
        return parsed;
      }
      parsed.options.listen.port = port;
    } else if (arg == "--data") {
      parsed.options.data_dir = std::filesystem::path{std::string{require_value(arg)}};
    } else if (arg == "--max-frame-bytes") {
      std::uint32_t max_frame_bytes{};
      if (!parse_u32(require_value(arg), max_frame_bytes)) {
        parsed.error = "invalid --max-frame-bytes value";
        return parsed;
      }
      parsed.options.max_frame_bytes = max_frame_bytes;
    } else if (arg == "--max-fetch-records") {
      std::uint32_t max_fetch_records{};
      if (!parse_u32(require_value(arg), max_fetch_records)) {
        parsed.error = "invalid --max-fetch-records value";
        return parsed;
      }
      parsed.options.max_fetch_records = max_fetch_records;
    } else if (arg == "--max-fetch-bytes") {
      std::uint32_t max_fetch_bytes{};
      if (!parse_u32(require_value(arg), max_fetch_bytes)) {
        parsed.error = "invalid --max-fetch-bytes value";
        return parsed;
      }
      parsed.options.max_fetch_bytes = max_fetch_bytes;
    } else if (arg == "--max-topic-partitions") {
      std::uint32_t max_topic_partitions{};
      if (!parse_u32(require_value(arg), max_topic_partitions) ||
          max_topic_partitions > std::numeric_limits<std::uint16_t>::max()) {
        parsed.error = "invalid --max-topic-partitions value";
        return parsed;
      }
      parsed.options.max_topic_partitions = max_topic_partitions;
    } else if (arg == "--max-fetch-wait-ms") {
      std::uint32_t max_fetch_wait_ms{};
      if (!parse_u32(require_value(arg), max_fetch_wait_ms)) {
        parsed.error = "invalid --max-fetch-wait-ms value";
        return parsed;
      }
      parsed.options.max_fetch_wait_ms = max_fetch_wait_ms;
    } else if (arg == "--max-append-queue-depth") {
      std::uint32_t max_append_queue_depth{};
      if (!parse_u32(require_value(arg), max_append_queue_depth, true)) {
        parsed.error = "invalid --max-append-queue-depth value";
        return parsed;
      }
      parsed.options.max_append_queue_depth = max_append_queue_depth;
    } else if (arg == "--append-workers") {
      std::uint32_t append_workers{};
      if (!parse_u32(require_value(arg), append_workers)) {
        parsed.error = "invalid --append-workers value";
        return parsed;
      }
      parsed.options.append_workers = append_workers;
    } else if (arg == "--max-broker-connections") {
      std::uint32_t max_broker_connections{};
      if (!parse_u32(require_value(arg), max_broker_connections)) {
        parsed.error = "invalid --max-broker-connections value";
        return parsed;
      }
      parsed.options.max_broker_connections = max_broker_connections;
    } else if (arg == "--max-long-poll-waiters") {
      std::uint32_t max_long_poll_waiters{};
      if (!parse_u32(require_value(arg), max_long_poll_waiters, true)) {
        parsed.error = "invalid --max-long-poll-waiters value";
        return parsed;
      }
      parsed.options.max_long_poll_waiters = max_long_poll_waiters;
    } else {
      parsed.error = "unknown argument: " + std::string{arg};
      return parsed;
    }
  }

  return parsed;
}

std::string server_usage() {
  return "Usage: boltstream-server [--listen HOST:PORT] [--port PORT] "
         "[--admin-listen HOST:PORT] [--data PATH] [--max-frame-bytes BYTES] "
         "[--max-fetch-records N] [--max-fetch-bytes BYTES] "
         "[--max-topic-partitions N] [--max-fetch-wait-ms MS] "
         "[--max-append-queue-depth N] [--append-workers N] "
         "[--max-broker-connections N] [--max-long-poll-waiters N]\n"
         "\n"
         "Defaults:\n"
         "  --listen 0.0.0.0:9000\n"
         "  --admin-listen 127.0.0.1:9100\n"
         "  --data ./data\n"
         "  --max-frame-bytes 1048576\n"
         "  --max-fetch-records 100\n"
         "  --max-fetch-bytes 1048576\n"
         "  --max-topic-partitions 128\n"
         "  --max-fetch-wait-ms 30000\n"
         "  --max-append-queue-depth 32\n"
         "  --append-workers 2\n"
         "  --max-broker-connections 128\n"
         "  --max-long-poll-waiters 128\n";
}

std::string endpoint_to_string(const Endpoint& endpoint) {
  std::ostringstream out;
  out << endpoint.host << ":" << endpoint.port;
  return out.str();
}

} // namespace boltstream::broker
