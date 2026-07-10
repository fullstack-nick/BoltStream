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
  std::uint32_t io_workers{1};
  std::filesystem::path data_dir{"./data"};
  std::uint32_t append_batch_records{1};
  std::uint32_t max_frame_bytes{1024 * 1024};
  std::uint32_t max_fetch_records{100};
  std::uint32_t max_fetch_bytes{1024 * 1024};
  std::uint32_t max_topic_partitions{128};
  std::uint32_t max_fetch_wait_ms{30000};
  std::uint32_t max_append_queue_depth{32};
  std::uint32_t append_workers{2};
  std::uint32_t max_broker_connections{128};
  std::uint32_t max_long_poll_waiters{128};
  std::uintmax_t segment_bytes{256ULL * 1024ULL * 1024ULL};
  std::uint64_t segment_max_age_seconds{60ULL * 60ULL};
  std::uint64_t retention_max_age_seconds{7ULL * 24ULL * 60ULL * 60ULL};
  std::uintmax_t retention_max_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint32_t retention_check_interval_ms{60000};
  bool metrics_enabled{true};
  std::string log_level{"info"};
  std::string log_format{"json"};
};

struct ParsedServerOptions {
  ServerOptions options;
  bool help_requested{false};
  bool version_requested{false};
  bool check_config_requested{false};
  bool print_effective_config_requested{false};
  std::filesystem::path config_path;
  bool config_loaded{false};
  std::string error;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

ParsedServerOptions parse_server_options(std::span<const std::string_view> args);
std::string server_usage();
std::string endpoint_to_string(const Endpoint& endpoint);

} // namespace boltstream::broker
