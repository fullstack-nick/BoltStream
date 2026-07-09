#include "boltstream/observability/logger.h"

#include "boltstream/broker/server.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace boltstream::observability {
namespace {

std::atomic<LogLevel>& configured_level() {
  static std::atomic<LogLevel> level{LogLevel::Info};
  return level;
}

std::mutex& logger_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::string& configured_git_sha() {
  static std::string git_sha{"unknown"};
  return git_sha;
}

LogLevel level_from_text(std::string_view value) {
  if (value == "debug") {
    return LogLevel::Debug;
  }
  if (value == "info") {
    return LogLevel::Info;
  }
  if (value == "warn") {
    return LogLevel::Warn;
  }
  if (value == "error") {
    return LogLevel::Error;
  }
  throw std::invalid_argument("invalid log level: " + std::string{value});
}

} // namespace

StructuredLogFields::StructuredLogFields(std::string level_in, std::string event_in,
                                         std::string remote_in, std::string frame_type_in,
                                         std::string error_code_in, std::string message_in,
                                         std::optional<std::uint64_t> correlation_id_in,
                                         std::optional<std::uint32_t> payload_bytes_in,
                                         std::optional<bool> retryable_in,
                                         std::optional<std::uint32_t> append_queue_depth_in,
                                         std::optional<std::uint32_t> waiter_count_in,
                                         std::optional<std::uint64_t> request_duration_us_in)
    : level(std::move(level_in)), event(std::move(event_in)), remote(std::move(remote_in)),
      frame_type(std::move(frame_type_in)), error_code(std::move(error_code_in)),
      message(std::move(message_in)), correlation_id(correlation_id_in),
      payload_bytes(payload_bytes_in), retryable(retryable_in),
      append_queue_depth(append_queue_depth_in), waiter_count(waiter_count_in),
      request_duration_us(request_duration_us_in) {}

LogLevel parse_log_level(std::string_view value) { return level_from_text(value); }

std::string_view log_level_name(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warn";
  case LogLevel::Error:
    return "error";
  }
  return "info";
}

void configure_logging(LogLevel minimum_level, std::string git_sha) {
  std::lock_guard lock{logger_mutex()};
  configured_git_sha() = std::move(git_sha);
  configured_level().store(minimum_level, std::memory_order_relaxed);
}

bool log_enabled(LogLevel level) {
  return static_cast<std::uint8_t>(level) >=
         static_cast<std::uint8_t>(configured_level().load(std::memory_order_relaxed));
}

std::string json_escape(std::string_view value) {
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

void write_structured_log(const StructuredLogFields& fields) {
  LogLevel level;
  try {
    level = level_from_text(fields.level);
  } catch (const std::invalid_argument&) {
    level = LogLevel::Info;
  }
  if (!log_enabled(level)) {
    return;
  }

  std::ostringstream out;
  out << "{";
  out << "\"timestamp\":\"" << broker::utc_now_iso8601() << "\"";
  out << ",\"level\":\"" << json_escape(fields.level) << "\"";
  out << ",\"event\":\"" << json_escape(fields.event) << "\"";
  out << ",\"component\":\"" << json_escape(fields.component) << "\"";
  {
    std::lock_guard lock{logger_mutex()};
    out << ",\"git_sha\":\"" << json_escape(configured_git_sha()) << "\"";
  }
  if (!fields.remote.empty()) {
    out << ",\"remote\":\"" << json_escape(fields.remote) << "\"";
  }
  if (fields.correlation_id) {
    out << ",\"correlation_id\":" << *fields.correlation_id;
  }
  if (!fields.frame_type.empty()) {
    out << ",\"frame_type\":\"" << json_escape(fields.frame_type) << "\"";
  }
  if (fields.payload_bytes) {
    out << ",\"payload_bytes\":" << *fields.payload_bytes;
  }
  if (!fields.error_code.empty()) {
    out << ",\"error_code\":\"" << json_escape(fields.error_code) << "\"";
  }
  if (fields.retryable) {
    out << ",\"retryable\":" << (*fields.retryable ? "true" : "false");
  }
  if (fields.append_queue_depth) {
    out << ",\"append_queue_depth\":" << *fields.append_queue_depth;
  }
  if (fields.waiter_count) {
    out << ",\"waiter_count\":" << *fields.waiter_count;
  }
  if (fields.request_duration_us) {
    out << ",\"request_duration_us\":" << *fields.request_duration_us;
  }
  for (const auto& [key, value] : fields.string_fields) {
    out << ",\"" << json_escape(key) << "\":\"" << json_escape(value) << "\"";
  }
  for (const auto& [key, value] : fields.numeric_fields) {
    out << ",\"" << json_escape(key) << "\":" << value;
  }
  if (!fields.message.empty()) {
    out << ",\"message\":\"" << json_escape(fields.message) << "\"";
  }
  out << "}\n";

  std::lock_guard lock{logger_mutex()};
  std::cerr << out.str();
}

} // namespace boltstream::observability
