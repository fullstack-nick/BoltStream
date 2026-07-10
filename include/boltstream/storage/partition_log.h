#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boltstream::storage {

inline constexpr std::uint16_t kRecordVersion = 1;
inline constexpr std::uint16_t kPhaseThreePartition = 0;
inline constexpr std::uintmax_t kDefaultMaxSegmentBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultSegmentMaxAgeSeconds = 60ULL * 60ULL;
inline constexpr std::uint64_t kDefaultRetentionMaxAgeSeconds = 7ULL * 24ULL * 60ULL * 60ULL;
inline constexpr std::uintmax_t kDefaultRetentionMaxBytes = 1024ULL * 1024ULL * 1024ULL;

struct PartitionLogOptions {
  std::filesystem::path data_dir{"./data"};
  std::string topic;
  std::uint16_t partition_id{kPhaseThreePartition};
  std::uintmax_t max_segment_bytes{kDefaultMaxSegmentBytes};
  std::uint64_t segment_max_age_seconds{kDefaultSegmentMaxAgeSeconds};
};

struct RecordMetadata {
  std::string topic;
  std::uint16_t partition{0};
  std::uint64_t offset{0};
  std::uint64_t timestamp_unix_ns{0};
  std::uint32_t encoded_byte_size{0};
};

struct Record {
  RecordMetadata metadata;
  std::vector<std::uint8_t> key;
  std::vector<std::uint8_t> value;
};

struct AppendRecord {
  std::span<const std::uint8_t> key;
  std::span<const std::uint8_t> value;
};

struct RecoveryStats {
  std::size_t segments_scanned{0};
  std::size_t indexes_rebuilt{0};
  std::size_t records_recovered{0};
  std::uintmax_t bytes_truncated{0};
  std::uint64_t next_offset{0};
};

struct StorageRecoverySummary {
  std::size_t topics_recovered{0};
  std::size_t partitions_recovered{0};
  std::size_t segments_scanned{0};
  std::size_t indexes_rebuilt{0};
  std::size_t records_recovered{0};
  std::uintmax_t bytes_truncated{0};
};

struct SegmentSummary {
  std::uint64_t base_offset{0};
  std::uint64_t first_offset{0};
  std::uint64_t last_offset{0};
  std::uintmax_t log_bytes{0};
  bool active{false};
  std::filesystem::path log_path;
};

struct RetentionPolicy {
  std::uint64_t max_age_seconds{kDefaultRetentionMaxAgeSeconds};
  std::uintmax_t max_bytes{kDefaultRetentionMaxBytes};
};

struct RetentionStats {
  std::size_t segments_deleted{0};
  std::uintmax_t bytes_deleted{0};
  std::size_t segments_retained{0};
  std::uintmax_t bytes_retained{0};
  std::uint64_t earliest_offset{0};
  std::uint64_t next_offset{0};
};

bool is_valid_topic_name(std::string_view topic);
std::filesystem::path partition_directory(const std::filesystem::path& data_dir,
                                          std::string_view topic, std::uint16_t partition_id);

class PartitionLog {
public:
  PartitionLog(const PartitionLog&) = delete;
  PartitionLog& operator=(const PartitionLog&) = delete;
  PartitionLog(PartitionLog&&) noexcept = default;
  PartitionLog& operator=(PartitionLog&&) noexcept = default;

  static PartitionLog open(PartitionLogOptions options);

  RecordMetadata append(std::span<const std::uint8_t> key, std::span<const std::uint8_t> value);
  std::vector<RecordMetadata> append_batch(std::span<const AppendRecord> records);
  std::vector<Record> read_from(std::uint64_t offset, std::size_t max_records,
                                std::uintmax_t max_bytes) const;
  RetentionStats apply_retention(const RetentionPolicy& policy);

  [[nodiscard]] std::uint64_t earliest_offset() const;
  [[nodiscard]] std::uint64_t next_offset() const { return next_offset_; }
  [[nodiscard]] std::vector<SegmentSummary> segment_summaries() const;
  [[nodiscard]] const RecoveryStats& recovery_stats() const { return recovery_stats_; }
  [[nodiscard]] const PartitionLogOptions& options() const { return options_; }

private:
  explicit PartitionLog(PartitionLogOptions options);

  struct IndexEntry {
    std::uint64_t offset{0};
    std::uint64_t file_position{0};
    std::uint32_t record_bytes{0};
    std::filesystem::path segment_path;
  };

  void recover();
  void open_active_segment();
  [[nodiscard]] std::filesystem::path segment_path(std::uint64_t base_offset) const;
  [[nodiscard]] std::filesystem::path index_path(std::uint64_t base_offset) const;
  [[nodiscard]] std::uint64_t active_segment_base() const;

  PartitionLogOptions options_;
  std::filesystem::path partition_dir_;
  std::vector<IndexEntry> index_;
  RecoveryStats recovery_stats_;
  std::uint64_t next_offset_{0};
};

StorageRecoverySummary recover_all_logs(const std::filesystem::path& data_dir,
                                        std::uintmax_t max_segment_bytes = kDefaultMaxSegmentBytes);

} // namespace boltstream::storage
