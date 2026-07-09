#pragma once

#include "boltstream/broker/options.h"

#include <filesystem>
#include <string>

namespace boltstream::config {

struct ConfigResult {
  bool ok{false};
  std::string error;
};

ConfigResult load_server_config(const std::filesystem::path& path, broker::ServerOptions& options);
std::string effective_config_yaml(const broker::ServerOptions& options, bool auth_required);

} // namespace boltstream::config
