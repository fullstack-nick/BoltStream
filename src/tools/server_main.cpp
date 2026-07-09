#include "boltstream/broker/options.h"
#include "boltstream/broker/server.h"
#include "boltstream/build_info.h"
#include "boltstream/config/config.h"
#include "boltstream/observability/logger.h"

#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

bool broker_token_configured() {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, "BOLTSTREAM_BROKER_TOKEN") != 0 || value == nullptr) {
    return false;
  }
  const bool configured = *value != '\0';
  std::free(value);
  return configured;
#else
  const auto* value = std::getenv("BOLTSTREAM_BROKER_TOKEN");
  return value != nullptr && *value != '\0';
#endif
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }

  const auto parsed =
      boltstream::broker::parse_server_options(std::span<const std::string_view>{args});
  if (!parsed.ok()) {
    std::cerr << parsed.error << "\n\n" << boltstream::broker::server_usage();
    return 2;
  }
  if (parsed.help_requested) {
    std::cout << boltstream::broker::server_usage();
    return 0;
  }

  const auto build_info = boltstream::current_build_info();
  if (parsed.version_requested) {
    std::cout << boltstream::build_info_json(build_info, boltstream::broker::utc_now_iso8601())
              << '\n';
    return 0;
  }

  const auto auth_required = broker_token_configured();
  if (parsed.print_effective_config_requested) {
    std::cout << boltstream::config::effective_config_yaml(parsed.options, auth_required);
    return 0;
  }
  if (parsed.check_config_requested) {
    std::cout << "configuration valid";
    if (parsed.config_loaded) {
      std::cout << ": " << parsed.config_path.string();
    }
    std::cout << '\n';
    return 0;
  }

  boltstream::observability::configure_logging(
      boltstream::observability::parse_log_level(parsed.options.log_level), build_info.git_sha);
  if (parsed.config_loaded) {
    boltstream::observability::StructuredLogFields fields{"info", "config_loaded"};
    fields.component = "config";
    fields.string_fields["path"] = parsed.config_path.string();
    fields.string_fields["listen"] = boltstream::broker::endpoint_to_string(parsed.options.listen);
    fields.string_fields["admin_listen"] =
        boltstream::broker::endpoint_to_string(parsed.options.admin_listen);
    fields.string_fields["data_dir"] = parsed.options.data_dir.string();
    fields.string_fields["log_level"] = parsed.options.log_level;
    fields.numeric_fields["metrics_enabled"] = parsed.options.metrics_enabled ? 1U : 0U;
    boltstream::observability::write_structured_log(fields);
  }

  try {
    boltstream::broker::BrokerServer server{parsed.options, build_info};
    server.start();
    server.wait_for_shutdown_signal();
  } catch (const std::exception& error) {
    std::cerr << "boltstream-server failed: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
