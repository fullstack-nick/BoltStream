#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boltstream::broker {

struct Endpoint {
  std::string host;
  std::uint16_t port;
};

struct ServerOptions {
  Endpoint listen{"0.0.0.0", 9000};
  Endpoint admin_listen{"127.0.0.1", 9100};
  std::filesystem::path data_dir{"./data"};
  std::uint32_t max_frame_bytes{1024 * 1024};
};

struct ParsedServerOptions {
  ServerOptions options;
  bool help_requested{false};
  bool version_requested{false};
  std::string error;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

ParsedServerOptions parse_server_options(std::span<const std::string_view> args);
std::string server_usage();
std::string endpoint_to_string(const Endpoint& endpoint);

} // namespace boltstream::broker
