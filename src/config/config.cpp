#include "boltstream/config/config.h"

#include <yaml-cpp/yaml.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>

namespace boltstream::config {
namespace {

std::string location(const YAML::Node& node) {
  const auto mark = node.Mark();
  if (mark.is_null()) {
    return {};
  }
  return " at line " + std::to_string(mark.line + 1) + ", column " +
         std::to_string(mark.column + 1);
}

ConfigResult failure(std::string message, const YAML::Node& node = {}) {
  return {false, std::move(message) + location(node)};
}

ConfigResult validate_mapping(const YAML::Node& node, std::string_view path,
                              const std::set<std::string>& allowed) {
  if (!node.IsMap()) {
    return failure(std::string{path} + " must be a mapping", node);
  }
  std::set<std::string> seen;
  for (const auto& item : node) {
    if (!item.first.IsScalar()) {
      return failure(std::string{path} + " keys must be scalars", item.first);
    }
    const auto key = item.first.Scalar();
    if (!seen.insert(key).second) {
      return failure("duplicate configuration key " + std::string{path} + "." + key, item.first);
    }
    if (!allowed.contains(key)) {
      return failure("unknown configuration key " + std::string{path} + "." + key, item.first);
    }
  }
  return {true, {}};
}

ConfigResult scalar_string(const YAML::Node& node, std::string_view path, std::string& value) {
  if (!node.IsScalar()) {
    return failure(std::string{path} + " must be a scalar", node);
  }
  value = node.Scalar();
  if (value.empty()) {
    return failure(std::string{path} + " must not be empty", node);
  }
  return {true, {}};
}

ConfigResult scalar_bool(const YAML::Node& node, std::string_view path, bool& value) {
  if (!node.IsScalar()) {
    return failure(std::string{path} + " must be a boolean", node);
  }
  try {
    value = node.as<bool>();
    return {true, {}};
  } catch (const YAML::Exception&) {
    return failure(std::string{path} + " must be true or false", node);
  }
}

template <typename T>
ConfigResult scalar_unsigned(const YAML::Node& node, std::string_view path, T& value,
                             bool allow_zero) {
  if (!node.IsScalar()) {
    return failure(std::string{path} + " must be an unsigned integer", node);
  }
  std::uint64_t parsed{};
  const auto text = node.Scalar();
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      (!allow_zero && parsed == 0) || parsed > std::numeric_limits<T>::max()) {
    return failure(std::string{path} + " is outside its allowed unsigned range", node);
  }
  value = static_cast<T>(parsed);
  return {true, {}};
}

ConfigResult parse_endpoint(std::string_view text, broker::Endpoint& endpoint,
                            std::string_view path, const YAML::Node& node) {
  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return failure(std::string{path} + " must be host:port", node);
  }
  std::uint32_t port{};
  const auto port_text = text.substr(colon + 1);
  const auto result = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
  if (result.ec != std::errc{} || result.ptr != port_text.data() + port_text.size() || port == 0 ||
      port > std::numeric_limits<std::uint16_t>::max()) {
    return failure(std::string{path} + " contains an invalid port", node);
  }
  endpoint.host = std::string{text.substr(0, colon)};
  endpoint.port = static_cast<std::uint16_t>(port);
  return {true, {}};
}

ConfigResult apply_server(const YAML::Node& node, broker::ServerOptions& options) {
  auto valid = validate_mapping(node, "server", {"listen", "admin_listen"});
  if (!valid.ok) {
    return valid;
  }
  for (const auto& item : node) {
    const auto key = item.first.Scalar();
    std::string value;
    auto parsed = scalar_string(item.second, "server." + key, value);
    if (!parsed.ok) {
      return parsed;
    }
    if (key == "listen") {
      parsed = parse_endpoint(value, options.listen, "server.listen", item.second);
    } else {
      parsed = parse_endpoint(value, options.admin_listen, "server.admin_listen", item.second);
    }
    if (!parsed.ok) {
      return parsed;
    }
  }
  return {true, {}};
}

ConfigResult apply_storage(const YAML::Node& node, broker::ServerOptions& options) {
  auto valid =
      validate_mapping(node, "storage", {"data_dir", "segment_bytes", "segment_max_age_seconds"});
  if (!valid.ok) {
    return valid;
  }
  for (const auto& item : node) {
    const auto key = item.first.Scalar();
    if (key == "data_dir") {
      std::string value;
      auto parsed = scalar_string(item.second, "storage.data_dir", value);
      if (!parsed.ok) {
        return parsed;
      }
      options.data_dir = value;
    } else if (key == "segment_bytes") {
      auto parsed =
          scalar_unsigned(item.second, "storage.segment_bytes", options.segment_bytes, false);
      if (!parsed.ok) {
        return parsed;
      }
    } else {
      auto parsed = scalar_unsigned(item.second, "storage.segment_max_age_seconds",
                                    options.segment_max_age_seconds, true);
      if (!parsed.ok) {
        return parsed;
      }
    }
  }
  return {true, {}};
}

ConfigResult apply_retention(const YAML::Node& node, broker::ServerOptions& options) {
  auto valid =
      validate_mapping(node, "retention", {"max_age_seconds", "max_bytes", "check_interval_ms"});
  if (!valid.ok) {
    return valid;
  }
  for (const auto& item : node) {
    const auto key = item.first.Scalar();
    ConfigResult parsed;
    if (key == "max_age_seconds") {
      parsed = scalar_unsigned(item.second, "retention.max_age_seconds",
                               options.retention_max_age_seconds, true);
    } else if (key == "max_bytes") {
      parsed =
          scalar_unsigned(item.second, "retention.max_bytes", options.retention_max_bytes, true);
    } else {
      parsed = scalar_unsigned(item.second, "retention.check_interval_ms",
                               options.retention_check_interval_ms, true);
    }
    if (!parsed.ok) {
      return parsed;
    }
  }
  return {true, {}};
}

ConfigResult apply_limits(const YAML::Node& node, broker::ServerOptions& options) {
  auto valid =
      validate_mapping(node, "limits",
                       {"max_frame_bytes", "max_fetch_records", "max_fetch_bytes",
                        "max_topic_partitions", "max_fetch_wait_ms", "max_append_queue_depth",
                        "append_workers", "max_broker_connections", "max_long_poll_waiters"});
  if (!valid.ok) {
    return valid;
  }
  for (const auto& item : node) {
    const auto key = item.first.Scalar();
    ConfigResult parsed;
    if (key == "max_frame_bytes") {
      parsed =
          scalar_unsigned(item.second, "limits.max_frame_bytes", options.max_frame_bytes, false);
    } else if (key == "max_fetch_records") {
      parsed = scalar_unsigned(item.second, "limits.max_fetch_records", options.max_fetch_records,
                               false);
    } else if (key == "max_fetch_bytes") {
      parsed =
          scalar_unsigned(item.second, "limits.max_fetch_bytes", options.max_fetch_bytes, false);
    } else if (key == "max_topic_partitions") {
      parsed = scalar_unsigned(item.second, "limits.max_topic_partitions",
                               options.max_topic_partitions, false);
      if (parsed.ok && options.max_topic_partitions > std::numeric_limits<std::uint16_t>::max()) {
        return failure("limits.max_topic_partitions exceeds 65535", item.second);
      }
    } else if (key == "max_fetch_wait_ms") {
      parsed = scalar_unsigned(item.second, "limits.max_fetch_wait_ms", options.max_fetch_wait_ms,
                               false);
    } else if (key == "max_append_queue_depth") {
      parsed = scalar_unsigned(item.second, "limits.max_append_queue_depth",
                               options.max_append_queue_depth, true);
    } else if (key == "append_workers") {
      parsed = scalar_unsigned(item.second, "limits.append_workers", options.append_workers, false);
    } else if (key == "max_broker_connections") {
      parsed = scalar_unsigned(item.second, "limits.max_broker_connections",
                               options.max_broker_connections, false);
    } else {
      parsed = scalar_unsigned(item.second, "limits.max_long_poll_waiters",
                               options.max_long_poll_waiters, true);
    }
    if (!parsed.ok) {
      return parsed;
    }
  }
  return {true, {}};
}

ConfigResult apply_metrics(const YAML::Node& node, broker::ServerOptions& options) {
  auto valid = validate_mapping(node, "metrics", {"enabled"});
  if (!valid.ok) {
    return valid;
  }
  if (const auto enabled = node["enabled"]) {
    return scalar_bool(enabled, "metrics.enabled", options.metrics_enabled);
  }
  return {true, {}};
}

ConfigResult apply_logging(const YAML::Node& node, broker::ServerOptions& options) {
  auto valid = validate_mapping(node, "logging", {"level", "format"});
  if (!valid.ok) {
    return valid;
  }
  if (const auto level = node["level"]) {
    auto parsed = scalar_string(level, "logging.level", options.log_level);
    if (!parsed.ok) {
      return parsed;
    }
    if (options.log_level != "debug" && options.log_level != "info" &&
        options.log_level != "warn" && options.log_level != "error") {
      return failure("logging.level must be debug, info, warn, or error", level);
    }
  }
  if (const auto format = node["format"]) {
    auto parsed = scalar_string(format, "logging.format", options.log_format);
    if (!parsed.ok) {
      return parsed;
    }
    if (options.log_format != "json") {
      return failure("logging.format must be json", format);
    }
  }
  return {true, {}};
}

} // namespace

ConfigResult load_server_config(const std::filesystem::path& path, broker::ServerOptions& options) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const YAML::Exception& error) {
    return {false, "failed to load config " + path.string() + ": " + error.what()};
  }
  auto valid = validate_mapping(root, "config",
                                {"server", "storage", "retention", "limits", "metrics", "logging"});
  if (!valid.ok) {
    return valid;
  }

  for (const auto& item : root) {
    const auto key = item.first.Scalar();
    ConfigResult applied;
    if (key == "server") {
      applied = apply_server(item.second, options);
    } else if (key == "storage") {
      applied = apply_storage(item.second, options);
    } else if (key == "retention") {
      applied = apply_retention(item.second, options);
    } else if (key == "limits") {
      applied = apply_limits(item.second, options);
    } else if (key == "metrics") {
      applied = apply_metrics(item.second, options);
    } else {
      applied = apply_logging(item.second, options);
    }
    if (!applied.ok) {
      return applied;
    }
  }
  return {true, {}};
}

std::string effective_config_yaml(const broker::ServerOptions& options, bool auth_required) {
  std::ostringstream out;
  out << "server:\n";
  out << "  listen: \"" << broker::endpoint_to_string(options.listen) << "\"\n";
  out << "  admin_listen: \"" << broker::endpoint_to_string(options.admin_listen) << "\"\n";
  out << "storage:\n";
  out << "  data_dir: \"" << options.data_dir.generic_string() << "\"\n";
  out << "  segment_bytes: " << options.segment_bytes << "\n";
  out << "  segment_max_age_seconds: " << options.segment_max_age_seconds << "\n";
  out << "retention:\n";
  out << "  max_age_seconds: " << options.retention_max_age_seconds << "\n";
  out << "  max_bytes: " << options.retention_max_bytes << "\n";
  out << "  check_interval_ms: " << options.retention_check_interval_ms << "\n";
  out << "limits:\n";
  out << "  max_frame_bytes: " << options.max_frame_bytes << "\n";
  out << "  max_fetch_records: " << options.max_fetch_records << "\n";
  out << "  max_fetch_bytes: " << options.max_fetch_bytes << "\n";
  out << "  max_topic_partitions: " << options.max_topic_partitions << "\n";
  out << "  max_fetch_wait_ms: " << options.max_fetch_wait_ms << "\n";
  out << "  max_append_queue_depth: " << options.max_append_queue_depth << "\n";
  out << "  append_workers: " << options.append_workers << "\n";
  out << "  max_broker_connections: " << options.max_broker_connections << "\n";
  out << "  max_long_poll_waiters: " << options.max_long_poll_waiters << "\n";
  out << "metrics:\n";
  out << "  enabled: " << (options.metrics_enabled ? "true" : "false") << "\n";
  out << "logging:\n";
  out << "  level: " << options.log_level << "\n";
  out << "  format: " << options.log_format << "\n";
  out << "auth_required: " << (auth_required ? "true" : "false") << "\n";
  return out.str();
}

} // namespace boltstream::config
