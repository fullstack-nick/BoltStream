#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace boltstream::storage {

struct OffsetStoreStats {
  std::size_t groups_recovered{0};
  std::size_t commits_recovered{0};
  std::uintmax_t bytes_truncated{0};
};

struct OffsetSnapshot {
  std::string group;
  std::string topic;
  std::uint16_t partition{0};
  std::uint64_t next_offset{0};
};

struct OffsetCleanupStats {
  std::size_t groups_touched{0};
  std::size_t offsets_removed{0};
};

bool is_valid_group_name(std::string_view group);

class OffsetStore {
public:
  static OffsetStore open(std::filesystem::path data_dir);

  [[nodiscard]] std::optional<std::uint64_t>
  committed(std::string_view group, std::string_view topic, std::uint16_t partition) const;
  [[nodiscard]] std::vector<OffsetSnapshot> group_offsets(std::string_view group,
                                                          std::string_view topic) const;
  [[nodiscard]] std::vector<OffsetSnapshot> all_offsets() const;
  void commit(std::string_view group, std::string_view topic, std::uint16_t partition,
              std::uint64_t next_offset);
  OffsetCleanupStats remove_topic(std::string_view topic);

  [[nodiscard]] const OffsetStoreStats& recovery_stats() const { return stats_; }

private:
  using OffsetKey = std::tuple<std::string, std::uint16_t>;
  using GroupOffsets = std::map<OffsetKey, std::uint64_t>;

  explicit OffsetStore(std::filesystem::path data_dir);

  void recover();
  void recover_group(std::string_view group, const std::filesystem::path& offsets_path);
  void rewrite_group(std::string_view group);
  [[nodiscard]] std::filesystem::path group_dir(std::string_view group) const;
  [[nodiscard]] std::filesystem::path offsets_path(std::string_view group) const;

  std::filesystem::path data_dir_;
  std::map<std::string, GroupOffsets> offsets_;
  OffsetStoreStats stats_;
};

} // namespace boltstream::storage
