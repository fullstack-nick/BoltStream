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
    } else {
      parsed.error = "unknown argument: " + std::string{arg};
      return parsed;
    }
  }

  return parsed;
}

std::string server_usage() {
  return "Usage: boltstream-server [--listen HOST:PORT] [--port PORT] "
         "[--admin-listen HOST:PORT] [--data PATH]\n"
         "\n"
         "Defaults:\n"
         "  --listen 0.0.0.0:9000\n"
         "  --admin-listen 127.0.0.1:9100\n"
         "  --data ./data\n";
}

std::string endpoint_to_string(const Endpoint& endpoint) {
  std::ostringstream out;
  out << endpoint.host << ":" << endpoint.port;
  return out.str();
}

} // namespace boltstream::broker
