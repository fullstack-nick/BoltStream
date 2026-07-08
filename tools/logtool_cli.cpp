#include "boltstream/storage/partition_log.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::string command;
  std::string data_dir;
  std::string topic;
  std::string key;
  std::string message;
  std::uint64_t from{0};
  std::size_t max_records{0};
  std::uint16_t partition{boltstream::storage::kPhaseThreePartition};
  std::uintmax_t segment_bytes{boltstream::storage::kDefaultMaxSegmentBytes};
  bool help_requested{false};
  bool key_seen{false};
  std::string error;
};

void usage() {
  std::cout << "Usage:\n"
               "  boltstream-logtool append --data PATH --topic TOPIC --key KEY --message VALUE\n"
               "                            [--segment-bytes BYTES] [--partition N]\n"
               "  boltstream-logtool read --data PATH --topic TOPIC --from OFFSET --max-records N\n"
               "                          [--segment-bytes BYTES] [--partition N]\n"
               "  boltstream-logtool recover --data PATH --topic TOPIC\n"
               "                             [--segment-bytes BYTES] [--partition N]\n";
}

template <typename UInt> bool parse_uint(std::string_view text, UInt& value) {
  UInt parsed{};
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  value = parsed;
  return true;
}

Options parse_options(int argc, char** argv) {
  Options options;
  if (argc <= 1) {
    options.error = "missing command";
    return options;
  }

  options.command = argv[1];
  if (options.command == "--help" || options.command == "-h") {
    options.help_requested = true;
    return options;
  }
  if (options.command != "append" && options.command != "read" && options.command != "recover") {
    options.error = "unknown command: " + options.command;
    return options;
  }

  for (int index = 2; index < argc; ++index) {
    const std::string_view arg{argv[index]};
    auto require_value = [&](std::string_view name) -> std::string_view {
      if (index + 1 >= argc) {
        options.error = "missing value for " + std::string{name};
        return {};
      }
      ++index;
      return argv[index];
    };

    if (arg == "--help" || arg == "-h") {
      options.help_requested = true;
    } else if (arg == "--data") {
      options.data_dir = std::string{require_value(arg)};
    } else if (arg == "--topic") {
      options.topic = std::string{require_value(arg)};
    } else if (arg == "--key") {
      options.key = std::string{require_value(arg)};
      options.key_seen = true;
    } else if (arg == "--message") {
      options.message = std::string{require_value(arg)};
    } else if (arg == "--from") {
      if (!parse_uint(require_value(arg), options.from)) {
        options.error = "invalid --from value";
        return options;
      }
    } else if (arg == "--max-records") {
      if (!parse_uint(require_value(arg), options.max_records) || options.max_records == 0) {
        options.error = "invalid --max-records value";
        return options;
      }
    } else if (arg == "--partition") {
      if (!parse_uint(require_value(arg), options.partition)) {
        options.error = "invalid --partition value";
        return options;
      }
    } else if (arg == "--segment-bytes") {
      if (!parse_uint(require_value(arg), options.segment_bytes) || options.segment_bytes == 0) {
        options.error = "invalid --segment-bytes value";
        return options;
      }
    } else {
      options.error = "unknown argument: " + std::string{arg};
      return options;
    }
  }

  if (options.help_requested) {
    return options;
  }
  if (options.data_dir.empty()) {
    options.error = "--data is required";
  } else if (options.topic.empty()) {
    options.error = "--topic is required";
  } else if (options.command == "append" && !options.key_seen) {
    options.error = "--key is required";
  } else if (options.command == "append" && options.message.empty()) {
    options.error = "--message is required";
  } else if (options.command == "read" && options.max_records == 0) {
    options.error = "--max-records is required";
  }
  return options;
}

std::string json_escape(std::string_view value) {
  std::string out;
  for (const char ch : value) {
    switch (ch) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20U) {
        out += "?";
      } else {
        out.push_back(ch);
      }
      break;
    }
  }
  return out;
}

std::string as_string(const std::vector<std::uint8_t>& bytes) {
  return {bytes.begin(), bytes.end()};
}

boltstream::storage::PartitionLog open_log(const Options& options) {
  return boltstream::storage::PartitionLog::open(
      {options.data_dir, options.topic, options.partition, options.segment_bytes});
}

int run_append(const Options& options) {
  auto log = open_log(options);
  const std::vector<std::uint8_t> key{options.key.begin(), options.key.end()};
  const std::vector<std::uint8_t> value{options.message.begin(), options.message.end()};
  const auto metadata = log.append(key, value);

  std::cout << "{\"status\":\"ok\",\"topic\":\"" << json_escape(metadata.topic)
            << "\",\"partition\":" << metadata.partition << ",\"offset\":" << metadata.offset
            << ",\"next_offset\":" << log.next_offset()
            << ",\"encoded_bytes\":" << metadata.encoded_byte_size << "}\n";
  return 0;
}

int run_read(const Options& options) {
  auto log = open_log(options);
  const auto records = log.read_from(options.from, options.max_records, 0);

  std::cout << "{\"status\":\"ok\",\"topic\":\"" << json_escape(options.topic)
            << "\",\"partition\":" << options.partition << ",\"from\":" << options.from
            << ",\"count\":" << records.size() << ",\"next_offset\":" << log.next_offset()
            << ",\"records\":[";
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    if (index > 0) {
      std::cout << ",";
    }
    std::cout << "{\"offset\":" << record.metadata.offset
              << ",\"timestamp_unix_ns\":" << record.metadata.timestamp_unix_ns << ",\"key\":\""
              << json_escape(as_string(record.key)) << "\",\"message\":\""
              << json_escape(as_string(record.value))
              << "\",\"encoded_bytes\":" << record.metadata.encoded_byte_size << "}";
  }
  std::cout << "]}\n";
  return 0;
}

int run_recover(const Options& options) {
  auto log = open_log(options);
  const auto& stats = log.recovery_stats();
  std::cout << "{\"status\":\"ok\",\"topic\":\"" << json_escape(options.topic)
            << "\",\"partition\":" << options.partition << ",\"next_offset\":" << log.next_offset()
            << ",\"segments_scanned\":" << stats.segments_scanned
            << ",\"indexes_rebuilt\":" << stats.indexes_rebuilt
            << ",\"records_recovered\":" << stats.records_recovered
            << ",\"bytes_truncated\":" << stats.bytes_truncated << "}\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (options.help_requested) {
    usage();
    return 0;
  }
  if (!options.error.empty()) {
    std::cerr << "boltstream-logtool: " << options.error << "\n\n";
    usage();
    return 2;
  }

  try {
    if (options.command == "append") {
      return run_append(options);
    }
    if (options.command == "read") {
      return run_read(options);
    }
    if (options.command == "recover") {
      return run_recover(options);
    }
  } catch (const std::exception& error) {
    std::cerr << "boltstream-logtool: " << error.what() << '\n';
    return 1;
  }
  return 2;
}
