#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace boltstream::observability {

enum class LogLevel : std::uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

struct StructuredLogFields {
  StructuredLogFields(std::string level_in, std::string event_in, std::string remote_in = {},
                      std::string frame_type_in = {}, std::string error_code_in = {},
                      std::string message_in = {},
                      std::optional<std::uint64_t> correlation_id_in = std::nullopt,
                      std::optional<std::uint32_t> payload_bytes_in = std::nullopt,
                      std::optional<bool> retryable_in = std::nullopt,
                      std::optional<std::uint32_t> append_queue_depth_in = std::nullopt,
                      std::optional<std::uint32_t> waiter_count_in = std::nullopt,
                      std::optional<std::uint64_t> request_duration_us_in = std::nullopt);

  std::string level{"info"};
  std::string event;
  std::string component{"broker"};
  std::string remote;
  std::string frame_type;
  std::string error_code;
  std::string message;
  std::optional<std::uint64_t> correlation_id;
  std::optional<std::uint32_t> payload_bytes;
  std::optional<bool> retryable;
  std::optional<std::uint32_t> append_queue_depth;
  std::optional<std::uint32_t> waiter_count;
  std::optional<std::uint64_t> request_duration_us;
  std::map<std::string, std::string> string_fields;
  std::map<std::string, std::uint64_t> numeric_fields;
};

LogLevel parse_log_level(std::string_view value);
std::string_view log_level_name(LogLevel level);
void configure_logging(LogLevel minimum_level, std::string git_sha);
bool log_enabled(LogLevel level);
void write_structured_log(const StructuredLogFields& fields);
std::string json_escape(std::string_view value);

} // namespace boltstream::observability
