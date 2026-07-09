#include "boltstream/broker/options.h"

#include "boltstream/config/config.h"

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

bool parse_u64(std::string_view text, std::uint64_t& value, bool allow_zero = false) {
  std::uint64_t parsed{};
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || (!allow_zero && parsed == 0)) {
    return false;
  }
  value = parsed;
  return true;
}

bool parse_umax(std::string_view text, std::uintmax_t& value, bool allow_zero = false) {
  std::uint64_t parsed{};
  if (!parse_u64(text, parsed, allow_zero)) {
    return false;
  }
  value = static_cast<std::uintmax_t>(parsed);
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

bool parse_bool(std::string_view text, bool& value) {
  if (text == "true") {
    value = true;
    return true;
  }
  if (text == "false") {
    value = false;
    return true;
  }
  return false;
}

} // namespace

ParsedServerOptions parse_server_options(std::span<const std::string_view> args) {
  ParsedServerOptions parsed;

  for (std::size_t index = 0; index < args.size(); ++index) {
    if (args[index] != "--config") {
      continue;
    }
    if (index + 1 >= args.size()) {
      parsed.error = "missing value for --config";
      return parsed;
    }
    if (!parsed.config_path.empty()) {
      parsed.error = "--config may be supplied only once";
      return parsed;
    }
    parsed.config_path = std::filesystem::path{std::string{args[++index]}};
  }
  if (!parsed.config_path.empty()) {
    const auto loaded = config::load_server_config(parsed.config_path, parsed.options);
    if (!loaded.ok) {
      parsed.error = loaded.error;
      return parsed;
    }
    parsed.config_loaded = true;
  }

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
    } else if (arg == "--check-config") {
      parsed.check_config_requested = true;
    } else if (arg == "--print-effective-config") {
      parsed.print_effective_config_requested = true;
    } else if (arg == "--config") {
      (void)require_value(arg);
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
    } else if (arg == "--segment-bytes") {
      std::uintmax_t segment_bytes{};
      if (!parse_umax(require_value(arg), segment_bytes)) {
        parsed.error = "invalid --segment-bytes value";
        return parsed;
      }
      parsed.options.segment_bytes = segment_bytes;
    } else if (arg == "--segment-max-age-seconds") {
      std::uint64_t segment_max_age_seconds{};
      if (!parse_u64(require_value(arg), segment_max_age_seconds, true)) {
        parsed.error = "invalid --segment-max-age-seconds value";
        return parsed;
      }
      parsed.options.segment_max_age_seconds = segment_max_age_seconds;
    } else if (arg == "--retention-max-age-seconds") {
      std::uint64_t retention_max_age_seconds{};
      if (!parse_u64(require_value(arg), retention_max_age_seconds, true)) {
        parsed.error = "invalid --retention-max-age-seconds value";
        return parsed;
      }
      parsed.options.retention_max_age_seconds = retention_max_age_seconds;
    } else if (arg == "--retention-max-bytes") {
      std::uintmax_t retention_max_bytes{};
      if (!parse_umax(require_value(arg), retention_max_bytes, true)) {
        parsed.error = "invalid --retention-max-bytes value";
        return parsed;
      }
      parsed.options.retention_max_bytes = retention_max_bytes;
    } else if (arg == "--retention-check-interval-ms") {
      std::uint32_t retention_check_interval_ms{};
      if (!parse_u32(require_value(arg), retention_check_interval_ms, true)) {
        parsed.error = "invalid --retention-check-interval-ms value";
        return parsed;
      }
      parsed.options.retention_check_interval_ms = retention_check_interval_ms;
    } else if (arg == "--metrics-enabled") {
      bool enabled{};
      if (!parse_bool(require_value(arg), enabled)) {
        parsed.error = "invalid --metrics-enabled value, expected true or false";
        return parsed;
      }
      parsed.options.metrics_enabled = enabled;
    } else if (arg == "--log-level") {
      const auto level = require_value(arg);
      if (level != "debug" && level != "info" && level != "warn" && level != "error") {
        parsed.error = "invalid --log-level value, expected debug, info, warn, or error";
        return parsed;
      }
      parsed.options.log_level = std::string{level};
    } else if (arg == "--log-format") {
      const auto format = require_value(arg);
      if (format != "json") {
        parsed.error = "invalid --log-format value, expected json";
        return parsed;
      }
      parsed.options.log_format = std::string{format};
    } else {
      parsed.error = "unknown argument: " + std::string{arg};
      return parsed;
    }
  }

  return parsed;
}

std::string server_usage() {
  return "Usage: boltstream-server [--config PATH] [--check-config] "
         "[--print-effective-config] [--listen HOST:PORT] [--port PORT] "
         "[--admin-listen HOST:PORT] [--data PATH] [--max-frame-bytes BYTES] "
         "[--max-fetch-records N] [--max-fetch-bytes BYTES] "
         "[--max-topic-partitions N] [--max-fetch-wait-ms MS] "
         "[--max-append-queue-depth N] [--append-workers N] "
         "[--max-broker-connections N] [--max-long-poll-waiters N] "
         "[--segment-bytes BYTES] [--segment-max-age-seconds SECONDS] "
         "[--retention-max-age-seconds SECONDS] [--retention-max-bytes BYTES] "
         "[--retention-check-interval-ms MS] [--metrics-enabled true|false] "
         "[--log-level debug|info|warn|error] [--log-format json]\n"
         "\n"
         "Precedence: compiled defaults < YAML config < explicit CLI flags.\n"
         "BOLTSTREAM_BROKER_TOKEN is environment-only and is never read from YAML.\n"
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
         "  --max-long-poll-waiters 128\n"
         "  --segment-bytes 268435456\n"
         "  --segment-max-age-seconds 3600\n"
         "  --retention-max-age-seconds 604800\n"
         "  --retention-max-bytes 1073741824\n"
         "  --retention-check-interval-ms 60000\n"
         "  --metrics-enabled true\n"
         "  --log-level info\n"
         "  --log-format json\n";
}

std::string endpoint_to_string(const Endpoint& endpoint) {
  std::ostringstream out;
  out << endpoint.host << ":" << endpoint.port;
  return out.str();
}

} // namespace boltstream::broker
