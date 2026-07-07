#pragma once

#include <string>

namespace boltstream {

struct BuildInfo {
  std::string service;
  std::string version;
  std::string git_sha;
  std::string build_type;
  std::string compiler;
  std::string protocol_version;
  std::string storage_format_version;
};

BuildInfo current_build_info();
std::string build_info_json(const BuildInfo& info, const std::string& startup_time_utc);

} // namespace boltstream
