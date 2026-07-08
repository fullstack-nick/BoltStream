#include "boltstream/storage/offset_store.h"
#include "boltstream/storage/partition_log.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct TempDir {
  std::filesystem::path path;

  TempDir() {
    path = std::filesystem::temp_directory_path() /
           ("boltstream-storage-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::vector<std::uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }

std::string text(const std::vector<std::uint8_t>& bytes) { return {bytes.begin(), bytes.end()}; }

boltstream::storage::PartitionLog
open_log(const std::filesystem::path& data_dir,
         std::uintmax_t max_segment_bytes = boltstream::storage::kDefaultMaxSegmentBytes) {
  return boltstream::storage::PartitionLog::open(
      {data_dir, "trades", boltstream::storage::kPhaseThreePartition, max_segment_bytes});
}

std::filesystem::path partition_dir(const std::filesystem::path& data_dir) {
  return boltstream::storage::partition_directory(data_dir, "trades",
                                                  boltstream::storage::kPhaseThreePartition);
}

std::vector<std::filesystem::path> files_with_extension(const std::filesystem::path& directory,
                                                        std::string_view extension) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == extension) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

void append_record(boltstream::storage::PartitionLog& log, std::string_view key,
                   std::string_view value) {
  const auto key_bytes = bytes(key);
  const auto value_bytes = bytes(value);
  (void)log.append(key_bytes, value_bytes);
}

} // namespace

TEST(StorageTests, AppendAndReadRoundTripAssignsOffsets) {
  TempDir temp;
  auto log = open_log(temp.path);

  append_record(log, "AAPL", "100");
  append_record(log, "MSFT", "200");
  append_record(log, "NVDA", "300");

  EXPECT_EQ(log.next_offset(), 3U);
  const auto records = log.read_from(0, 10, 0);

  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].metadata.offset, 0U);
  EXPECT_EQ(records[1].metadata.offset, 1U);
  EXPECT_EQ(records[2].metadata.offset, 2U);
  EXPECT_EQ(records[0].metadata.topic, "trades");
  EXPECT_EQ(records[0].metadata.partition, 0U);
  EXPECT_EQ(text(records[1].key), "MSFT");
  EXPECT_EQ(text(records[2].value), "300");
}

TEST(StorageTests, ReopenRecoversRecordsAndNextOffset) {
  TempDir temp;
  {
    auto log = open_log(temp.path);
    append_record(log, "AAPL", "100");
    append_record(log, "MSFT", "200");
  }

  auto recovered = open_log(temp.path);
  const auto records = recovered.read_from(0, 10, 0);

  EXPECT_EQ(recovered.next_offset(), 2U);
  EXPECT_EQ(recovered.recovery_stats().records_recovered, 2U);
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(text(records[0].value), "100");
  EXPECT_EQ(text(records[1].value), "200");
}

TEST(StorageTests, SegmentRollCreatesMultipleSegmentsAndReadsAcrossThem) {
  TempDir temp;
  auto log = open_log(temp.path, 80);

  append_record(log, "A", "message-00000000000000000000000000000000");
  append_record(log, "B", "message-11111111111111111111111111111111");
  append_record(log, "C", "message-22222222222222222222222222222222");

  const auto logs = files_with_extension(partition_dir(temp.path), ".log");
  const auto indexes = files_with_extension(partition_dir(temp.path), ".index");
  EXPECT_GE(logs.size(), 2U);
  EXPECT_EQ(logs.size(), indexes.size());

  const auto records = log.read_from(0, 10, 0);
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(text(records[0].key), "A");
  EXPECT_EQ(text(records[1].key), "B");
  EXPECT_EQ(text(records[2].key), "C");
}

TEST(StorageTests, SegmentRollsWhenActiveSegmentExceedsMaxAge) {
  TempDir temp;
  auto log = boltstream::storage::PartitionLog::open({temp.path, "trades", 0, 1024, 1});

  append_record(log, "A", "first");
  const auto first_segment = files_with_extension(partition_dir(temp.path), ".log").front();
  std::filesystem::last_write_time(first_segment, std::filesystem::file_time_type::clock::now() -
                                                      std::chrono::seconds(5));
  append_record(log, "B", "second");

  const auto logs = files_with_extension(partition_dir(temp.path), ".log");
  ASSERT_EQ(logs.size(), 2U);
  const auto summaries = log.segment_summaries();
  ASSERT_EQ(summaries.size(), 2U);
  EXPECT_FALSE(summaries[0].active);
  EXPECT_TRUE(summaries[1].active);
}

TEST(StorageTests, RetentionDeletesOnlyInactiveSegmentsAndRecoversLowWatermark) {
  TempDir temp;
  {
    auto log = open_log(temp.path, 96);
    append_record(log, "A", "message-00000000000000000000000000000000");
    append_record(log, "B", "message-11111111111111111111111111111111");
    append_record(log, "C", "message-22222222222222222222222222222222");

    ASSERT_GE(files_with_extension(partition_dir(temp.path), ".log").size(), 3U);
    const auto stats = log.apply_retention({0, 100});
    EXPECT_GE(stats.segments_deleted, 1U);
    EXPECT_EQ(stats.earliest_offset, 2U);
    EXPECT_EQ(stats.next_offset, 3U);
    EXPECT_EQ(log.earliest_offset(), 2U);
    const auto remaining = log.read_from(log.earliest_offset(), 10, 0);
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_EQ(remaining[0].metadata.offset, 2U);
    EXPECT_EQ(text(remaining[0].key), "C");
  }

  auto recovered = open_log(temp.path, 96);
  EXPECT_EQ(recovered.earliest_offset(), 2U);
  EXPECT_EQ(recovered.next_offset(), 3U);
  const auto records = recovered.read_from(recovered.earliest_offset(), 10, 0);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(text(records[0].key), "C");
}

TEST(StorageTests, RebuildsMissingIndexesFromLogs) {
  TempDir temp;
  {
    auto log = open_log(temp.path);
    append_record(log, "AAPL", "100");
    append_record(log, "MSFT", "200");
  }

  for (const auto& index : files_with_extension(partition_dir(temp.path), ".index")) {
    std::filesystem::remove(index);
  }

  auto recovered = open_log(temp.path);
  const auto indexes = files_with_extension(partition_dir(temp.path), ".index");
  const auto records = recovered.read_from(0, 10, 0);

  ASSERT_FALSE(indexes.empty());
  EXPECT_EQ(recovered.recovery_stats().indexes_rebuilt, 1U);
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(text(records[1].key), "MSFT");
}

TEST(StorageTests, TruncatesIncompleteTrailingBytes) {
  TempDir temp;
  {
    auto log = open_log(temp.path);
    append_record(log, "AAPL", "100");
    append_record(log, "MSFT", "200");
  }

  const auto log_path = files_with_extension(partition_dir(temp.path), ".log").front();
  const auto valid_size = std::filesystem::file_size(log_path);
  {
    const std::vector<std::uint8_t> trailing{0x12, 0x34};
    std::ofstream out{log_path, std::ios::binary | std::ios::app};
    out.write(reinterpret_cast<const char*>(trailing.data()),
              static_cast<std::streamsize>(trailing.size()));
  }

  auto recovered = open_log(temp.path);
  const auto records = recovered.read_from(0, 10, 0);

  EXPECT_EQ(recovered.recovery_stats().bytes_truncated, 2U);
  EXPECT_EQ(std::filesystem::file_size(log_path), valid_size);
  ASSERT_EQ(records.size(), 2U);
}

TEST(StorageTests, TruncatesCrcCorruptedTrailingRecord) {
  TempDir temp;
  {
    auto log = open_log(temp.path);
    append_record(log, "AAPL", "100");
    append_record(log, "MSFT", "200");
  }

  const auto log_path = files_with_extension(partition_dir(temp.path), ".log").front();
  const auto original_size = std::filesystem::file_size(log_path);
  {
    std::fstream file{log_path, std::ios::binary | std::ios::in | std::ios::out};
    file.seekg(static_cast<std::streamoff>(original_size - 1));
    char byte = 0;
    file.read(&byte, 1);
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0xFFU);
    file.seekp(static_cast<std::streamoff>(original_size - 1));
    file.write(&byte, 1);
  }

  auto recovered = open_log(temp.path);
  const auto records = recovered.read_from(0, 10, 0);

  EXPECT_GT(recovered.recovery_stats().bytes_truncated, 0U);
  EXPECT_LT(std::filesystem::file_size(log_path), original_size);
  EXPECT_EQ(recovered.next_offset(), 1U);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(text(records[0].key), "AAPL");
}

TEST(StorageTests, RejectsInvalidTopicNames) {
  TempDir temp;

  for (const auto* topic : {"", ".", "..", "../bad", "bad/name", "bad\\name", "bad name"}) {
    EXPECT_THROW((void)boltstream::storage::PartitionLog::open(
                     {temp.path, topic, boltstream::storage::kPhaseThreePartition,
                      boltstream::storage::kDefaultMaxSegmentBytes}),
                 std::invalid_argument)
        << topic;
  }
}

TEST(StorageTests, OpensAndPersistsNonZeroPartition) {
  TempDir temp;
  auto log = boltstream::storage::PartitionLog::open(
      {temp.path, "trades", 2, boltstream::storage::kDefaultMaxSegmentBytes});

  append_record(log, "AAPL", "100");

  EXPECT_EQ(log.next_offset(), 1U);
  const auto records = log.read_from(0, 10, 0);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].metadata.partition, 2U);
  EXPECT_TRUE(
      std::filesystem::exists(boltstream::storage::partition_directory(temp.path, "trades", 2)));
}

TEST(StorageTests, OffsetStoreReplaysCommittedOffsets) {
  TempDir temp;
  {
    auto store = boltstream::storage::OffsetStore::open(temp.path);
    store.commit("dashboard", "trades", 2, 11);
  }

  auto recovered = boltstream::storage::OffsetStore::open(temp.path);

  const auto offset = recovered.committed("dashboard", "trades", 2);
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, 11U);
  EXPECT_EQ(recovered.recovery_stats().commits_recovered, 1U);
}

TEST(StorageTests, OffsetStoreTruncatesCorruptTrailingBytes) {
  TempDir temp;
  {
    auto store = boltstream::storage::OffsetStore::open(temp.path);
    store.commit("dashboard", "trades", 0, 7);
  }

  const auto log_path = temp.path / "consumer_offsets" / "dashboard" / "offsets.log";
  const auto valid_size = std::filesystem::file_size(log_path);
  {
    std::ofstream out{log_path, std::ios::binary | std::ios::app};
    out << "corrupt-trailing-record";
  }

  auto recovered = boltstream::storage::OffsetStore::open(temp.path);

  const auto offset = recovered.committed("dashboard", "trades", 0);
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, 7U);
  EXPECT_EQ(std::filesystem::file_size(log_path), valid_size);
  EXPECT_GT(recovered.recovery_stats().bytes_truncated, 0U);
}

TEST(StorageTests, OffsetStoreRemovesTopicOffsetsDurably) {
  TempDir temp;
  {
    auto store = boltstream::storage::OffsetStore::open(temp.path);
    store.commit("dashboard", "trades", 0, 7);
    store.commit("dashboard", "quotes", 0, 3);
    store.commit("audit", "trades", 1, 9);

    const auto stats = store.remove_topic("trades");
    EXPECT_EQ(stats.groups_touched, 2U);
    EXPECT_EQ(stats.offsets_removed, 2U);
  }

  auto recovered = boltstream::storage::OffsetStore::open(temp.path);
  EXPECT_FALSE(recovered.committed("dashboard", "trades", 0).has_value());
  const auto kept = recovered.committed("dashboard", "quotes", 0);
  ASSERT_TRUE(kept.has_value());
  EXPECT_EQ(*kept, 3U);
  EXPECT_FALSE(recovered.committed("audit", "trades", 1).has_value());
}
