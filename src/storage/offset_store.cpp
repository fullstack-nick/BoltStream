#include "boltstream/storage/offset_store.h"

#include "boltstream/protocol/protocol.h"
#include "boltstream/storage/partition_log.h"

#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace boltstream::storage {
namespace {

std::string crc_input(std::string_view topic, std::uint16_t partition, std::uint64_t next_offset) {
  std::ostringstream out;
  out << topic << '\t' << partition << '\t' << next_offset;
  return out.str();
}

void write_offset_record(std::ostream& out, std::string_view topic, std::uint16_t partition,
                         std::uint64_t next_offset) {
  const auto input = crc_input(topic, partition, next_offset);
  const auto crc = protocol::crc32(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(input.data()), input.size()});
  out << input << '\t' << crc << '\n';
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

bool parse_offset_line(std::string_view line, std::string& topic, std::uint16_t& partition,
                       std::uint64_t& next_offset) {
  const auto first_tab = line.find('\t');
  if (first_tab == std::string_view::npos) {
    return false;
  }
  const auto second_tab = line.find('\t', first_tab + 1);
  if (second_tab == std::string_view::npos) {
    return false;
  }
  const auto third_tab = line.find('\t', second_tab + 1);
  if (third_tab == std::string_view::npos ||
      line.find('\t', third_tab + 1) != std::string_view::npos) {
    return false;
  }

  topic = std::string{line.substr(0, first_tab)};
  if (!is_valid_topic_name(topic)) {
    return false;
  }

  unsigned int parsed_partition = 0;
  if (!parse_uint(line.substr(first_tab + 1, second_tab - first_tab - 1), parsed_partition) ||
      parsed_partition > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  partition = static_cast<std::uint16_t>(parsed_partition);

  if (!parse_uint(line.substr(second_tab + 1, third_tab - second_tab - 1), next_offset)) {
    return false;
  }

  std::uint32_t expected_crc = 0;
  if (!parse_uint(line.substr(third_tab + 1), expected_crc)) {
    return false;
  }

  const auto input = crc_input(topic, partition, next_offset);
  return protocol::crc32(std::span<const std::uint8_t>{
             reinterpret_cast<const std::uint8_t*>(input.data()), input.size()}) == expected_crc;
}

void truncate_file(const std::filesystem::path& path, std::uintmax_t size) {
  std::error_code ec;
  std::filesystem::resize_file(path, size, ec);
  if (ec) {
    throw std::runtime_error("failed to truncate offset log " + path.string() + ": " +
                             ec.message());
  }
}

} // namespace

bool is_valid_group_name(std::string_view group) { return is_valid_topic_name(group); }

OffsetStore OffsetStore::open(std::filesystem::path data_dir) {
  OffsetStore store{std::move(data_dir)};
  store.recover();
  return store;
}

OffsetStore::OffsetStore(std::filesystem::path data_dir) : data_dir_(std::move(data_dir)) {}

std::optional<std::uint64_t> OffsetStore::committed(std::string_view group, std::string_view topic,
                                                    std::uint16_t partition) const {
  const auto group_it = offsets_.find(std::string{group});
  if (group_it == offsets_.end()) {
    return std::nullopt;
  }
  const auto offset_it = group_it->second.find({std::string{topic}, partition});
  if (offset_it == group_it->second.end()) {
    return std::nullopt;
  }
  return offset_it->second;
}

std::vector<OffsetSnapshot> OffsetStore::group_offsets(std::string_view group,
                                                       std::string_view topic) const {
  std::vector<OffsetSnapshot> snapshots;
  const auto group_it = offsets_.find(std::string{group});
  if (group_it == offsets_.end()) {
    return snapshots;
  }
  for (const auto& [key, next_offset] : group_it->second) {
    const auto& [offset_topic, partition] = key;
    if (offset_topic != topic) {
      continue;
    }
    snapshots.push_back({std::string{group}, offset_topic, partition, next_offset});
  }
  return snapshots;
}

void OffsetStore::commit(std::string_view group, std::string_view topic, std::uint16_t partition,
                         std::uint64_t next_offset) {
  if (!is_valid_group_name(group)) {
    throw std::invalid_argument("invalid consumer group name");
  }
  if (!is_valid_topic_name(topic)) {
    throw std::invalid_argument("invalid topic name");
  }

  std::error_code ec;
  std::filesystem::create_directories(group_dir(group), ec);
  if (ec) {
    throw std::runtime_error("failed to create consumer offset directory: " + ec.message());
  }

  std::ofstream out{offsets_path(group), std::ios::binary | std::ios::app};
  if (!out) {
    throw std::runtime_error("failed to open consumer offset log");
  }
  write_offset_record(out, topic, partition, next_offset);
  out.flush();
  if (!out) {
    throw std::runtime_error("failed to append consumer offset log");
  }

  offsets_[std::string{group}][{std::string{topic}, partition}] = next_offset;
}

OffsetCleanupStats OffsetStore::remove_topic(std::string_view topic) {
  OffsetCleanupStats stats;
  std::vector<std::string> touched_groups;
  for (auto& [group, offsets] : offsets_) {
    const auto before = offsets.size();
    for (auto it = offsets.begin(); it != offsets.end();) {
      const auto& [offset_topic, partition] = it->first;
      (void)partition;
      if (offset_topic == topic) {
        it = offsets.erase(it);
      } else {
        ++it;
      }
    }
    const auto removed = before - offsets.size();
    if (removed > 0) {
      stats.offsets_removed += removed;
      ++stats.groups_touched;
      touched_groups.push_back(group);
    }
  }

  for (const auto& group : touched_groups) {
    rewrite_group(group);
  }
  return stats;
}

void OffsetStore::recover() {
  stats_ = {};
  offsets_.clear();

  const auto offsets_dir = data_dir_ / "consumer_offsets";
  std::error_code ec;
  std::filesystem::create_directories(offsets_dir, ec);
  if (ec) {
    throw std::runtime_error("failed to create consumer offsets directory: " + ec.message());
  }

  for (const auto& entry : std::filesystem::directory_iterator(offsets_dir)) {
    if (!entry.is_directory()) {
      continue;
    }
    const auto group = entry.path().filename().string();
    if (!is_valid_group_name(group)) {
      continue;
    }
    const auto path = entry.path() / "offsets.log";
    if (!std::filesystem::exists(path, ec)) {
      continue;
    }
    recover_group(group, path);
    ++stats_.groups_recovered;
  }
}

void OffsetStore::recover_group(std::string_view group, const std::filesystem::path& path) {
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    throw std::runtime_error("failed to open consumer offset log for recovery: " + path.string());
  }
  const std::string content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

  std::uintmax_t valid_end = 0;
  std::size_t position = 0;
  while (position < content.size()) {
    const auto newline = content.find('\n', position);
    if (newline == std::string::npos) {
      break;
    }

    auto line = std::string_view{content.data() + position, newline - position};
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    std::string topic;
    std::uint16_t partition = 0;
    std::uint64_t next_offset = 0;
    if (!parse_offset_line(line, topic, partition, next_offset)) {
      break;
    }

    offsets_[std::string{group}][{std::move(topic), partition}] = next_offset;
    ++stats_.commits_recovered;
    valid_end = static_cast<std::uintmax_t>(newline + 1);
    position = newline + 1;
  }

  if (valid_end < content.size()) {
    stats_.bytes_truncated += static_cast<std::uintmax_t>(content.size()) - valid_end;
    truncate_file(path, valid_end);
  }
}

void OffsetStore::rewrite_group(std::string_view group) {
  const auto group_it = offsets_.find(std::string{group});
  if (group_it == offsets_.end() || group_it->second.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(group_dir(group), ec);
    if (ec) {
      throw std::runtime_error("failed to remove empty consumer offset directory: " + ec.message());
    }
    if (group_it != offsets_.end()) {
      offsets_.erase(group_it);
    }
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(group_dir(group), ec);
  if (ec) {
    throw std::runtime_error("failed to create consumer offset directory: " + ec.message());
  }

  const auto path = offsets_path(group);
  const auto tmp_path = path.string() + ".tmp";
  {
    std::ofstream out{tmp_path, std::ios::binary | std::ios::trunc};
    if (!out) {
      throw std::runtime_error("failed to rewrite consumer offset log: " + tmp_path);
    }
    for (const auto& [key, next_offset] : group_it->second) {
      const auto& [topic, partition] = key;
      write_offset_record(out, topic, partition, next_offset);
    }
    out.flush();
    if (!out) {
      throw std::runtime_error("failed to flush rewritten consumer offset log: " + tmp_path);
    }
  }

  std::filesystem::remove(path, ec);
  if (ec) {
    throw std::runtime_error("failed to replace consumer offset log: " + ec.message());
  }
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    throw std::runtime_error("failed to install rewritten consumer offset log: " + ec.message());
  }
}

std::filesystem::path OffsetStore::group_dir(std::string_view group) const {
  return data_dir_ / "consumer_offsets" / std::string{group};
}

std::filesystem::path OffsetStore::offsets_path(std::string_view group) const {
  return group_dir(group) / "offsets.log";
}

} // namespace boltstream::storage
