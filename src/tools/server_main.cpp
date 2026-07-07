#include "boltstream/broker/options.h"
#include "boltstream/broker/server.h"
#include "boltstream/build_info.h"

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

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
