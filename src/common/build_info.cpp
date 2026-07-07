#include "boltstream/build_info.h"

#include "boltstream/build_config.h"

#include <sstream>

namespace boltstream {
namespace {

std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (const char ch : value) {
    switch (ch) {
    case '\\':
      out << "\\\\";
      break;
    case '"':
      out << "\\\"";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      out << ch;
      break;
    }
  }
  return out.str();
}

} // namespace

BuildInfo current_build_info() {
  return BuildInfo{
      .service = "boltstream",
      .version = BOLTSTREAM_VERSION,
      .git_sha = BOLTSTREAM_GIT_SHA,
      .build_type = BOLTSTREAM_BUILD_TYPE,
      .compiler = BOLTSTREAM_COMPILER,
      .protocol_version = BOLTSTREAM_PROTOCOL_VERSION,
      .storage_format_version = BOLTSTREAM_STORAGE_FORMAT_VERSION,
  };
}

std::string build_info_json(const BuildInfo& info, const std::string& startup_time_utc) {
  std::ostringstream out;
  out << "{";
  out << "\"service\":\"" << json_escape(info.service) << "\",";
  out << "\"version\":\"" << json_escape(info.version) << "\",";
  out << "\"git_sha\":\"" << json_escape(info.git_sha) << "\",";
  out << "\"build_type\":\"" << json_escape(info.build_type) << "\",";
  out << "\"compiler\":\"" << json_escape(info.compiler) << "\",";
  out << "\"protocol_version\":\"" << json_escape(info.protocol_version) << "\",";
  out << "\"storage_format_version\":\"" << json_escape(info.storage_format_version) << "\",";
  out << "\"startup_time_utc\":\"" << json_escape(startup_time_utc) << "\"";
  out << "}";
  return out.str();
}

} // namespace boltstream
