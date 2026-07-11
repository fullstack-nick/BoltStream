#include "boltstream/storage/partition_log.h"

#include "boltstream/protocol/protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace boltstream::storage {
namespace {

constexpr std::uint16_t kRecordFlags = 0;
constexpr std::uint16_t kBatchVersion = 2;
constexpr std::uint32_t kFixedBatchBodyBytes = 36;
constexpr std::uint32_t kHeaderCount = 0;
constexpr std::uint32_t kFixedRecordBodyBytes = 32;
constexpr std::uint32_t kMinimumRecordBytes = kFixedRecordBodyBytes + sizeof(std::uint32_t);
constexpr std::uint32_t kIndexEntryBytes = 20;

struct SegmentInfo {
  std::uint64_t base_offset{0};
  std::filesystem::path log_path;
};

struct DecodedRecord {
  Record record;
  std::uint32_t record_bytes{0};
};

struct DecodedBatch {
  BatchMetadata metadata;
  std::vector<protocol::BatchRecord> records;
  std::uint32_t batch_bytes{0};
};

void write_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void write_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

std::uint16_t read_u16_be(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                    static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t read_u32_be(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_u64_be(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
    value = (value << 8U) | static_cast<std::uint64_t>(bytes[offset + index]);
  }
  return value;
}

void write_index_entry(std::ofstream& out, std::uint64_t offset, std::uint64_t file_position,
                       std::uint32_t record_bytes) {
  std::vector<std::uint8_t> entry;
  entry.reserve(kIndexEntryBytes);
  write_u64(entry, offset);
  write_u64(entry, file_position);
  write_u32(entry, record_bytes);
  out.write(reinterpret_cast<const char*>(entry.data()),
            static_cast<std::streamsize>(entry.size()));
}

std::uint64_t unix_time_ns() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::string format_base_offset(std::uint64_t base_offset) {
  std::ostringstream out;
  out << std::setw(20) << std::setfill('0') << base_offset;
  return out.str();
}

bool parse_base_offset(const std::filesystem::path& path, std::uint64_t& base_offset) {
  const auto name = path.filename().string();
  if (name.size() != 24 || name.substr(20) != ".log") {
    return false;
  }
  for (std::size_t index = 0; index < 20; ++index) {
    if (name[index] < '0' || name[index] > '9') {
      return false;
    }
  }

  const auto* begin = name.data();
  const auto* end = name.data() + 20;
  const auto parsed = std::from_chars(begin, end, base_offset);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_partition_id(const std::filesystem::path& path, std::uint16_t& partition_id) {
  const auto name = path.filename().string();
  constexpr std::string_view prefix{"partition-"};
  if (name.size() != prefix.size() + 6 || name.rfind(prefix, 0) != 0) {
    return false;
  }
  for (std::size_t index = prefix.size(); index < name.size(); ++index) {
    if (name[index] < '0' || name[index] > '9') {
      return false;
    }
  }

  unsigned int parsed = 0;
  const auto* begin = name.data() + prefix.size();
  const auto* end = name.data() + name.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end ||
      parsed > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  partition_id = static_cast<std::uint16_t>(parsed);
  return true;
}

std::vector<SegmentInfo> list_segments(const std::filesystem::path& partition_dir) {
  std::vector<SegmentInfo> segments;
  std::error_code ec;
  if (!std::filesystem::exists(partition_dir, ec)) {
    return segments;
  }
  for (const auto& entry : std::filesystem::directory_iterator(partition_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::uint64_t base_offset = 0;
    if (parse_base_offset(entry.path(), base_offset)) {
      segments.push_back({base_offset, entry.path()});
    }
  }
  std::sort(segments.begin(), segments.end(), [](const auto& left, const auto& right) {
    return left.base_offset < right.base_offset;
  });
  return segments;
}

DecodedRecord decode_record(std::string_view topic, std::uint16_t partition,
                            std::uint32_t record_bytes,
                            std::span<const std::uint8_t> body_and_crc) {
  if (record_bytes != body_and_crc.size() || record_bytes < kMinimumRecordBytes) {
    throw std::runtime_error("invalid record length");
  }

  const auto body_bytes = static_cast<std::size_t>(record_bytes - sizeof(std::uint32_t));
  const auto expected_crc = read_u32_be(body_and_crc, body_bytes);
  const auto actual_crc =
      protocol::crc32(std::span<const std::uint8_t>{body_and_crc.data(), body_bytes});
  if (expected_crc != actual_crc) {
    throw std::runtime_error("record crc mismatch");
  }

  const auto version = read_u16_be(body_and_crc, 0);
  const auto flags = read_u16_be(body_and_crc, 2);
  const auto offset = read_u64_be(body_and_crc, 4);
  const auto timestamp_unix_ns = read_u64_be(body_and_crc, 12);
  const auto key_bytes = read_u32_be(body_and_crc, 20);
  const auto value_bytes = read_u32_be(body_and_crc, 24);
  const auto header_count = read_u32_be(body_and_crc, 28);

  if (version != kRecordVersion || flags != kRecordFlags || header_count != kHeaderCount) {
    throw std::runtime_error("unsupported record header");
  }

  const auto expected_body = static_cast<std::uint64_t>(kFixedRecordBodyBytes) +
                             static_cast<std::uint64_t>(key_bytes) +
                             static_cast<std::uint64_t>(value_bytes);
  if (expected_body != body_bytes) {
    throw std::runtime_error("record payload length mismatch");
  }

  DecodedRecord decoded;
  decoded.record_bytes = record_bytes;
  decoded.record.metadata.topic = std::string{topic};
  decoded.record.metadata.partition = partition;
  decoded.record.metadata.offset = offset;
  decoded.record.metadata.timestamp_unix_ns = timestamp_unix_ns;
  decoded.record.metadata.encoded_byte_size =
      record_bytes + static_cast<std::uint32_t>(sizeof(std::uint32_t));

  const auto key_begin = body_and_crc.begin() + kFixedRecordBodyBytes;
  const auto value_begin = key_begin + static_cast<std::ptrdiff_t>(key_bytes);
  decoded.record.key.assign(key_begin, value_begin);
  decoded.record.value.assign(value_begin, value_begin + static_cast<std::ptrdiff_t>(value_bytes));
  return decoded;
}

std::vector<std::uint8_t> encode_record(std::uint64_t offset, std::uint64_t timestamp_unix_ns,
                                        std::span<const std::uint8_t> key,
                                        std::span<const std::uint8_t> value) {
  if (key.size() > std::numeric_limits<std::uint32_t>::max() ||
      value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("record key/value exceed format limits");
  }

  std::vector<std::uint8_t> body;
  body.reserve(kFixedRecordBodyBytes + key.size() + value.size());
  write_u16(body, kRecordVersion);
  write_u16(body, kRecordFlags);
  write_u64(body, offset);
  write_u64(body, timestamp_unix_ns);
  write_u32(body, static_cast<std::uint32_t>(key.size()));
  write_u32(body, static_cast<std::uint32_t>(value.size()));
  write_u32(body, kHeaderCount);
  body.insert(body.end(), key.begin(), key.end());
  body.insert(body.end(), value.begin(), value.end());

  const auto record_bytes = body.size() + sizeof(std::uint32_t);
  if (record_bytes > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("record exceeds format limits");
  }

  std::vector<std::uint8_t> encoded;
  encoded.reserve(sizeof(std::uint32_t) + record_bytes);
  write_u32(encoded, static_cast<std::uint32_t>(record_bytes));
  encoded.insert(encoded.end(), body.begin(), body.end());
  write_u32(encoded, protocol::crc32(body));
  return encoded;
}

std::vector<std::uint8_t> encode_batch(std::uint64_t base_offset, std::uint64_t timestamp_unix_ns,
                                       compression::Codec codec, std::uint32_t record_count,
                                       std::uint32_t uncompressed_bytes,
                                       std::span<const std::uint8_t> encoded_records) {
  std::vector<std::uint8_t> body;
  body.reserve(kFixedBatchBodyBytes + encoded_records.size());
  write_u16(body, kBatchVersion);
  write_u16(body, 0);
  write_u16(body, static_cast<std::uint16_t>(codec));
  write_u16(body, 0);
  write_u64(body, base_offset);
  write_u64(body, timestamp_unix_ns);
  write_u32(body, record_count);
  write_u32(body, uncompressed_bytes);
  write_u32(body, static_cast<std::uint32_t>(encoded_records.size()));
  body.insert(body.end(), encoded_records.begin(), encoded_records.end());
  std::vector<std::uint8_t> out;
  write_u32(out, static_cast<std::uint32_t>(body.size() + sizeof(std::uint32_t)));
  out.insert(out.end(), body.begin(), body.end());
  write_u32(out, protocol::crc32(body));
  return out;
}

DecodedBatch decode_batch(std::string_view topic, std::uint16_t partition,
                          std::uint32_t batch_bytes, std::span<const std::uint8_t> body_and_crc,
                          std::size_t maximum_uncompressed = protocol::kDefaultMaxFrameBytes) {
  if (batch_bytes != body_and_crc.size() || batch_bytes < kFixedBatchBodyBytes + 4U) {
    throw std::runtime_error("invalid batch length");
  }
  const auto body_bytes = batch_bytes - 4U;
  if (protocol::crc32(body_and_crc.first(body_bytes)) != read_u32_be(body_and_crc, body_bytes)) {
    throw std::runtime_error("batch crc mismatch");
  }
  if (read_u16_be(body_and_crc, 0) != kBatchVersion || read_u16_be(body_and_crc, 2) != 0 ||
      read_u16_be(body_and_crc, 6) != 0) {
    throw std::runtime_error("unsupported batch header");
  }
  const auto codec = static_cast<compression::Codec>(read_u16_be(body_and_crc, 4));
  const auto base_offset = read_u64_be(body_and_crc, 8);
  const auto timestamp = read_u64_be(body_and_crc, 16);
  const auto count = read_u32_be(body_and_crc, 24);
  const auto uncompressed = read_u32_be(body_and_crc, 28);
  const auto encoded = read_u32_be(body_and_crc, 32);
  if (!compression::is_supported(codec) || count == 0 || uncompressed == 0 ||
      encoded != body_bytes - kFixedBatchBodyBytes) {
    throw std::runtime_error("invalid batch header");
  }
  const auto raw =
      compression::decompress(codec, body_and_crc.subspan(kFixedBatchBodyBytes, encoded),
                              uncompressed, maximum_uncompressed);
  DecodedBatch result;
  const auto decoded = protocol::decode_record_set(raw, count, result.records);
  if (!decoded.ok) {
    throw std::runtime_error(decoded.message);
  }
  result.batch_bytes = batch_bytes;
  result.metadata = {std::string{topic},
                     partition,
                     base_offset,
                     base_offset + count,
                     timestamp,
                     count,
                     uncompressed,
                     encoded,
                     batch_bytes + static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                     codec};
  return result;
}

std::uintmax_t file_size_or_zero(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  return ec ? 0 : size;
}

std::uint64_t file_age_seconds_or_zero(const std::filesystem::path& path) {
  std::error_code ec;
  const auto modified = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return 0;
  }
  const auto now = decltype(modified)::clock::now();
  if (modified >= now) {
    return 0;
  }
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now - modified).count());
}

void truncate_file(const std::filesystem::path& path, std::uintmax_t size) {
  std::error_code ec;
  std::filesystem::resize_file(path, size, ec);
  if (ec) {
    throw std::runtime_error("failed to truncate " + path.string() + ": " + ec.message());
  }
}

} // namespace

bool is_valid_topic_name(std::string_view topic) {
  if (topic.empty() || topic == "." || topic == "..") {
    return false;
  }
  return std::all_of(topic.begin(), topic.end(), [](char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '.' || ch == '_' || ch == '-';
  });
}

std::filesystem::path partition_directory(const std::filesystem::path& data_dir,
                                          std::string_view topic, std::uint16_t partition_id) {
  std::ostringstream partition;
  partition << "partition-" << std::setw(6) << std::setfill('0') << partition_id;
  return data_dir / "topics" / std::string{topic} / partition.str();
}

PartitionLog PartitionLog::open(PartitionLogOptions options) {
  PartitionLog log{std::move(options)};
  log.recover();
  log.open_active_segment();
  return log;
}

PartitionLog::PartitionLog(PartitionLogOptions options) : options_(std::move(options)) {
  if (!is_valid_topic_name(options_.topic)) {
    throw std::invalid_argument("invalid topic name");
  }
  if (options_.max_segment_bytes == 0) {
    throw std::invalid_argument("max_segment_bytes must be greater than zero");
  }
  partition_dir_ = partition_directory(options_.data_dir, options_.topic, options_.partition_id);
}

RecordMetadata PartitionLog::append(std::span<const std::uint8_t> key,
                                    std::span<const std::uint8_t> value) {
  const AppendRecord record{key, value};
  auto appended = append_batch(std::span<const AppendRecord>{&record, 1});
  return std::move(appended.front());
}

std::vector<RecordMetadata> PartitionLog::append_batch(std::span<const AppendRecord> records) {
  if (records.empty()) {
    return {};
  }

  struct OriginalFiles {
    std::filesystem::path log_path;
    std::filesystem::path index_path;
    std::uintmax_t log_bytes{0};
    std::uintmax_t index_bytes{0};
  };
  std::map<std::uint64_t, OriginalFiles> originals;
  for (const auto& segment : list_segments(partition_dir_)) {
    const auto index_file = index_path(segment.base_offset);
    originals.emplace(segment.base_offset, OriginalFiles{segment.log_path, index_file,
                                                         file_size_or_zero(segment.log_path),
                                                         file_size_or_zero(index_file)});
  }

  const auto original_index_size = index_.size();
  const auto original_next_offset = next_offset_;
  std::vector<IndexEntry> staged_index;
  std::vector<RecordMetadata> metadata;
  staged_index.reserve(records.size());
  metadata.reserve(records.size());

  std::ofstream log_file;
  std::ofstream index_file;
  std::uint64_t base_offset = active_segment_base();
  auto active_log_path = segment_path(base_offset);
  auto current_size = file_size_or_zero(active_log_path);

  auto flush_chunk = [&] {
    if (!log_file.is_open()) {
      return;
    }
    log_file.flush();
    index_file.flush();
    if (!log_file || !index_file) {
      throw std::runtime_error("failed to flush append batch for segment " +
                               active_log_path.string());
    }
    log_file.close();
    index_file.close();
  };

  auto open_chunk = [&] {
    active_log_path = segment_path(base_offset);
    log_file.open(active_log_path, std::ios::binary | std::ios::app);
    if (!log_file) {
      throw std::runtime_error("failed to open segment for append: " + active_log_path.string());
    }
    index_file.open(index_path(base_offset), std::ios::binary | std::ios::app);
    if (!index_file) {
      throw std::runtime_error("failed to open index for append: " +
                               index_path(base_offset).string());
    }
  };

  try {
    for (std::size_t record_index = 0; record_index < records.size(); ++record_index) {
      const auto offset = original_next_offset + record_index;
      const auto timestamp = unix_time_ns();
      auto encoded =
          encode_record(offset, timestamp, records[record_index].key, records[record_index].value);
      const auto age_roll_due =
          options_.segment_max_age_seconds > 0 && current_size > 0 &&
          file_age_seconds_or_zero(active_log_path) >= options_.segment_max_age_seconds;
      const auto size_roll_due =
          current_size > 0 && current_size + encoded.size() > options_.max_segment_bytes;
      if (size_roll_due || age_roll_due) {
        flush_chunk();
        base_offset = offset;
        active_log_path = segment_path(base_offset);
        current_size = 0;
      }
      if (!log_file.is_open()) {
        open_chunk();
      }

      const auto record_bytes = static_cast<std::uint32_t>(encoded.size() - sizeof(std::uint32_t));
      log_file.write(reinterpret_cast<const char*>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size()));
      write_index_entry(index_file, offset, static_cast<std::uint64_t>(current_size), record_bytes);
      if (!log_file || !index_file) {
        throw std::runtime_error("failed to write append batch for segment " +
                                 active_log_path.string());
      }

      staged_index.push_back(
          {offset, static_cast<std::uint64_t>(current_size), record_bytes, active_log_path});
      metadata.push_back({options_.topic, options_.partition_id, offset, timestamp,
                          static_cast<std::uint32_t>(encoded.size())});
      current_size += encoded.size();
    }
    flush_chunk();
  } catch (...) {
    log_file.close();
    index_file.close();
    std::error_code ignored;
    for (const auto& segment : list_segments(partition_dir_)) {
      const auto original = originals.find(segment.base_offset);
      if (original == originals.end()) {
        std::filesystem::remove(segment.log_path, ignored);
        std::filesystem::remove(index_path(segment.base_offset), ignored);
      } else {
        std::filesystem::resize_file(original->second.log_path, original->second.log_bytes,
                                     ignored);
        ignored.clear();
        std::filesystem::resize_file(original->second.index_path, original->second.index_bytes,
                                     ignored);
      }
    }
    index_.resize(original_index_size);
    next_offset_ = original_next_offset;
    throw;
  }

  index_.insert(index_.end(), std::make_move_iterator(staged_index.begin()),
                std::make_move_iterator(staged_index.end()));
  next_offset_ += records.size();
  return metadata;
}

BatchMetadata PartitionLog::append_encoded_batch(compression::Codec codec,
                                                 std::span<const std::uint8_t> encoded_records,
                                                 std::uint32_t uncompressed_bytes,
                                                 std::span<const AppendRecord> decoded_records) {
  if (decoded_records.empty() ||
      decoded_records.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("encoded batch record count is invalid");
  }
  std::vector<protocol::BatchRecord> validation_records;
  validation_records.reserve(decoded_records.size());
  for (const auto& record : decoded_records) {
    validation_records.push_back(
        {{record.key.begin(), record.key.end()}, {record.value.begin(), record.value.end()}});
  }
  const auto canonical = protocol::encode_record_set(validation_records);
  if (canonical.size() != uncompressed_bytes) {
    throw std::invalid_argument("encoded batch logical size mismatch");
  }
  const auto decoded = compression::decompress(codec, encoded_records, uncompressed_bytes,
                                               options_.max_uncompressed_batch_bytes);
  if (decoded != canonical) {
    throw std::invalid_argument("encoded batch content mismatch");
  }

  const auto base_offset = next_offset_;
  const auto timestamp = unix_time_ns();
  const auto encoded = encode_batch(base_offset, timestamp, codec,
                                    static_cast<std::uint32_t>(decoded_records.size()),
                                    uncompressed_bytes, encoded_records);
  auto segment_base = active_segment_base();
  auto log_path = segment_path(segment_base);
  auto current_size = file_size_or_zero(log_path);
  if (current_size > 0 && current_size + encoded.size() > options_.max_segment_bytes) {
    segment_base = base_offset;
    log_path = segment_path(segment_base);
    current_size = 0;
  }
  const auto index_file_path = index_path(segment_base);
  const auto original_log_size = current_size;
  const auto original_index_size = file_size_or_zero(index_file_path);
  try {
    std::ofstream log_file{log_path, std::ios::binary | std::ios::app};
    std::ofstream index_file{index_file_path, std::ios::binary | std::ios::app};
    if (!log_file || !index_file) {
      throw std::runtime_error("failed to open encoded batch segment");
    }
    log_file.write(reinterpret_cast<const char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
    const auto body_bytes = static_cast<std::uint32_t>(encoded.size() - 4U);
    for (std::uint32_t index = 0; index < decoded_records.size(); ++index) {
      write_index_entry(index_file, base_offset + index, current_size, body_bytes);
    }
    log_file.flush();
    index_file.flush();
    if (!log_file || !index_file) {
      throw std::runtime_error("failed to flush encoded batch");
    }
    for (std::uint32_t index = 0; index < decoded_records.size(); ++index) {
      index_.push_back({base_offset + index, current_size, body_bytes, log_path, index,
                        static_cast<std::uint32_t>(decoded_records.size())});
    }
    next_offset_ += decoded_records.size();
  } catch (...) {
    std::error_code ignored;
    if (original_log_size == 0) {
      std::filesystem::remove(log_path, ignored);
    } else {
      std::filesystem::resize_file(log_path, original_log_size, ignored);
    }
    ignored.clear();
    if (original_index_size == 0) {
      std::filesystem::remove(index_file_path, ignored);
    } else {
      std::filesystem::resize_file(index_file_path, original_index_size, ignored);
    }
    throw;
  }
  return {options_.topic,
          options_.partition_id,
          base_offset,
          next_offset_,
          timestamp,
          static_cast<std::uint32_t>(decoded_records.size()),
          uncompressed_bytes,
          static_cast<std::uint32_t>(encoded_records.size()),
          static_cast<std::uint32_t>(encoded.size()),
          codec};
}

std::vector<Record> PartitionLog::read_from(std::uint64_t offset, std::size_t max_records,
                                            std::uintmax_t max_bytes) const {
  std::vector<Record> records;
  std::uintmax_t emitted_bytes = 0;
  for (const auto& entry : index_) {
    if (entry.offset < offset) {
      continue;
    }
    if (records.size() >= max_records) {
      break;
    }
    if (max_bytes > 0 && !records.empty() &&
        emitted_bytes + entry.record_bytes + sizeof(std::uint32_t) > max_bytes) {
      break;
    }

    std::ifstream segment{entry.segment_path, std::ios::binary};
    if (!segment) {
      throw std::runtime_error("failed to open segment for read: " + entry.segment_path.string());
    }
    segment.seekg(static_cast<std::streamoff>(entry.file_position));

    std::array<std::uint8_t, sizeof(std::uint32_t)> length{};
    segment.read(reinterpret_cast<char*>(length.data()),
                 static_cast<std::streamsize>(length.size()));
    if (!segment) {
      throw std::runtime_error("failed to read record length");
    }
    const auto record_bytes = read_u32_be(length, 0);
    std::vector<std::uint8_t> body_and_crc(record_bytes);
    segment.read(reinterpret_cast<char*>(body_and_crc.data()),
                 static_cast<std::streamsize>(body_and_crc.size()));
    if (!segment) {
      throw std::runtime_error("failed to read record body");
    }

    if (read_u16_be(body_and_crc, 0) == kBatchVersion) {
      auto batch = decode_batch(options_.topic, options_.partition_id, record_bytes, body_and_crc,
                                options_.max_uncompressed_batch_bytes);
      if (entry.record_index >= batch.records.size()) {
        throw std::runtime_error("batch index entry is out of range");
      }
      Record record;
      record.metadata = {options_.topic, options_.partition_id, entry.offset,
                         batch.metadata.timestamp_unix_ns,
                         batch.metadata.stored_bytes / batch.metadata.record_count};
      record.key = std::move(batch.records[entry.record_index].key);
      record.value = std::move(batch.records[entry.record_index].message);
      records.push_back(std::move(record));
    } else {
      auto decoded =
          decode_record(options_.topic, options_.partition_id, record_bytes, body_and_crc);
      records.push_back(std::move(decoded.record));
    }
    emitted_bytes += record_bytes + sizeof(std::uint32_t);
  }
  return records;
}

std::optional<EncodedBatch> PartitionLog::read_encoded_batch(std::uint64_t offset) const {
  const auto entry = std::find_if(index_.begin(), index_.end(),
                                  [offset](const auto& item) { return item.offset == offset; });
  if (entry == index_.end() || entry->record_index != 0) {
    return std::nullopt;
  }
  std::ifstream segment{entry->segment_path, std::ios::binary};
  if (!segment) {
    throw std::runtime_error("failed to open encoded batch segment");
  }
  segment.seekg(static_cast<std::streamoff>(entry->file_position + sizeof(std::uint32_t)));
  std::vector<std::uint8_t> body_and_crc(entry->record_bytes);
  segment.read(reinterpret_cast<char*>(body_and_crc.data()),
               static_cast<std::streamsize>(body_and_crc.size()));
  if (!segment || read_u16_be(body_and_crc, 0) != kBatchVersion) {
    return std::nullopt;
  }
  auto decoded = decode_batch(options_.topic, options_.partition_id, entry->record_bytes,
                              body_and_crc, options_.max_uncompressed_batch_bytes);
  EncodedBatch result;
  result.metadata = decoded.metadata;
  result.encoded_records.assign(
      body_and_crc.begin() + static_cast<std::ptrdiff_t>(kFixedBatchBodyBytes),
      body_and_crc.begin() +
          static_cast<std::ptrdiff_t>(kFixedBatchBodyBytes + decoded.metadata.encoded_bytes));
  return result;
}

RetentionStats PartitionLog::apply_retention(const RetentionPolicy& policy) {
  RetentionStats stats;
  auto summaries = segment_summaries();

  std::uintmax_t total_bytes = 0;
  for (const auto& summary : summaries) {
    total_bytes += summary.log_bytes;
  }

  std::set<std::filesystem::path> deleted_logs;
  for (const auto& summary : summaries) {
    if (summary.active) {
      continue;
    }

    const auto delete_by_age = policy.max_age_seconds > 0 &&
                               file_age_seconds_or_zero(summary.log_path) >= policy.max_age_seconds;
    const auto delete_by_size = policy.max_bytes > 0 && total_bytes > policy.max_bytes;
    if (!delete_by_age && !delete_by_size) {
      continue;
    }

    std::error_code ec;
    std::filesystem::remove(summary.log_path, ec);
    if (ec) {
      throw std::runtime_error("failed to delete retained segment " + summary.log_path.string() +
                               ": " + ec.message());
    }
    std::filesystem::remove(index_path(summary.base_offset), ec);
    if (ec) {
      throw std::runtime_error("failed to delete retained index " +
                               index_path(summary.base_offset).string() + ": " + ec.message());
    }

    deleted_logs.insert(summary.log_path);
    ++stats.segments_deleted;
    stats.bytes_deleted += summary.log_bytes;
    total_bytes = summary.log_bytes > total_bytes ? 0 : total_bytes - summary.log_bytes;
  }

  if (!deleted_logs.empty()) {
    index_.erase(std::remove_if(
                     index_.begin(), index_.end(),
                     [&](const auto& entry) { return deleted_logs.contains(entry.segment_path); }),
                 index_.end());
  }

  summaries = segment_summaries();
  for (const auto& summary : summaries) {
    ++stats.segments_retained;
    stats.bytes_retained += summary.log_bytes;
  }
  stats.earliest_offset = earliest_offset();
  stats.next_offset = next_offset_;
  return stats;
}

std::uint64_t PartitionLog::earliest_offset() const {
  if (index_.empty()) {
    return next_offset_;
  }
  return index_.front().offset;
}

std::vector<SegmentSummary> PartitionLog::segment_summaries() const {
  std::map<std::filesystem::path, SegmentSummary> by_path;
  const auto active_base = active_segment_base();
  for (const auto& segment : list_segments(partition_dir_)) {
    auto& summary = by_path[segment.log_path];
    summary.base_offset = segment.base_offset;
    summary.first_offset = segment.base_offset;
    summary.last_offset = segment.base_offset;
    summary.log_bytes = file_size_or_zero(segment.log_path);
    summary.active = segment.base_offset == active_base;
    summary.log_path = segment.log_path;
  }

  for (const auto& entry : index_) {
    auto& summary = by_path[entry.segment_path];
    if (summary.log_path.empty()) {
      std::uint64_t base_offset = 0;
      (void)parse_base_offset(entry.segment_path, base_offset);
      summary.base_offset = base_offset;
      summary.first_offset = entry.offset;
      summary.last_offset = entry.offset;
      summary.log_bytes = file_size_or_zero(entry.segment_path);
      summary.active = base_offset == active_base;
      summary.log_path = entry.segment_path;
    } else {
      summary.first_offset = std::min(summary.first_offset, entry.offset);
      summary.last_offset = std::max(summary.last_offset, entry.offset);
    }
  }

  std::vector<SegmentSummary> summaries;
  summaries.reserve(by_path.size());
  for (auto& [_, summary] : by_path) {
    summaries.push_back(std::move(summary));
  }
  std::sort(summaries.begin(), summaries.end(), [](const auto& left, const auto& right) {
    return left.base_offset < right.base_offset;
  });
  return summaries;
}

void PartitionLog::recover() {
  std::error_code ec;
  std::filesystem::create_directories(partition_dir_, ec);
  if (ec) {
    throw std::runtime_error("failed to create partition directory: " + ec.message());
  }

  index_.clear();
  recovery_stats_ = {};
  next_offset_ = 0;

  const auto segments = list_segments(partition_dir_);
  bool stop_after_corruption = false;
  for (std::size_t segment_index = 0; segment_index < segments.size(); ++segment_index) {
    const auto& segment = segments[segment_index];
    const auto index_file_path = index_path(segment.base_offset);

    if (stop_after_corruption) {
      recovery_stats_.bytes_truncated += file_size_or_zero(segment.log_path);
      std::filesystem::remove(segment.log_path, ec);
      std::filesystem::remove(index_file_path, ec);
      continue;
    }

    ++recovery_stats_.segments_scanned;
    ++recovery_stats_.indexes_rebuilt;
    const auto original_size = file_size_or_zero(segment.log_path);
    std::uintmax_t position = 0;
    bool truncate = false;

    std::ifstream in{segment.log_path, std::ios::binary};
    if (!in) {
      throw std::runtime_error("failed to open segment for recovery: " + segment.log_path.string());
    }
    std::ofstream rebuilt_index{index_file_path, std::ios::binary | std::ios::trunc};
    if (!rebuilt_index) {
      throw std::runtime_error("failed to rebuild index: " + index_file_path.string());
    }

    while (position < original_size) {
      if (original_size - position < sizeof(std::uint32_t)) {
        truncate = true;
        break;
      }

      std::array<std::uint8_t, sizeof(std::uint32_t)> length{};
      in.seekg(static_cast<std::streamoff>(position));
      in.read(reinterpret_cast<char*>(length.data()), static_cast<std::streamsize>(length.size()));
      if (!in) {
        truncate = true;
        break;
      }

      const auto record_bytes = read_u32_be(length, 0);
      if (record_bytes < kMinimumRecordBytes ||
          static_cast<std::uintmax_t>(record_bytes) >
              original_size - position - sizeof(std::uint32_t)) {
        truncate = true;
        break;
      }

      std::vector<std::uint8_t> body_and_crc(record_bytes);
      in.read(reinterpret_cast<char*>(body_and_crc.data()),
              static_cast<std::streamsize>(body_and_crc.size()));
      if (!in) {
        truncate = true;
        break;
      }

      try {
        if (read_u16_be(body_and_crc, 0) == kBatchVersion) {
          const auto batch = decode_batch(options_.topic, options_.partition_id, record_bytes,
                                          body_and_crc, options_.max_uncompressed_batch_bytes);
          for (std::uint32_t index = 0; index < batch.metadata.record_count; ++index) {
            const auto offset = batch.metadata.base_offset + index;
            write_index_entry(rebuilt_index, offset, static_cast<std::uint64_t>(position),
                              record_bytes);
            index_.push_back({offset, static_cast<std::uint64_t>(position), record_bytes,
                              segment.log_path, index, batch.metadata.record_count});
          }
          next_offset_ = std::max(next_offset_, batch.metadata.next_offset);
          recovery_stats_.records_recovered += batch.metadata.record_count;
        } else {
          const auto decoded =
              decode_record(options_.topic, options_.partition_id, record_bytes, body_and_crc);
          write_index_entry(rebuilt_index, decoded.record.metadata.offset,
                            static_cast<std::uint64_t>(position), record_bytes);
          index_.push_back({decoded.record.metadata.offset, static_cast<std::uint64_t>(position),
                            record_bytes, segment.log_path});
          next_offset_ = std::max(next_offset_, decoded.record.metadata.offset + 1);
          ++recovery_stats_.records_recovered;
        }
      } catch (const std::exception&) {
        truncate = true;
        break;
      }

      position += sizeof(std::uint32_t) + static_cast<std::uintmax_t>(record_bytes);
    }

    rebuilt_index.flush();
    in.close();
    rebuilt_index.close();

    if (truncate) {
      truncate_file(segment.log_path, position);
      recovery_stats_.bytes_truncated += original_size - position;
      stop_after_corruption = true;
    }
  }

  recovery_stats_.next_offset = next_offset_;
  std::sort(index_.begin(), index_.end(),
            [](const auto& left, const auto& right) { return left.offset < right.offset; });
}

void PartitionLog::open_active_segment() {
  std::ofstream segment{segment_path(active_segment_base()), std::ios::binary | std::ios::app};
  if (!segment) {
    throw std::runtime_error("failed to open active segment");
  }
  std::ofstream index_file{index_path(active_segment_base()), std::ios::binary | std::ios::app};
  if (!index_file) {
    throw std::runtime_error("failed to open active index");
  }
}

std::filesystem::path PartitionLog::segment_path(std::uint64_t base_offset) const {
  return partition_dir_ / (format_base_offset(base_offset) + ".log");
}

std::filesystem::path PartitionLog::index_path(std::uint64_t base_offset) const {
  return partition_dir_ / (format_base_offset(base_offset) + ".index");
}

std::uint64_t PartitionLog::active_segment_base() const {
  if (index_.empty()) {
    return 0;
  }
  std::uint64_t base_offset = 0;
  if (!parse_base_offset(index_.back().segment_path, base_offset)) {
    return next_offset_;
  }
  return base_offset;
}

StorageRecoverySummary recover_all_logs(const std::filesystem::path& data_dir,
                                        std::uintmax_t max_segment_bytes) {
  StorageRecoverySummary summary;
  const auto topics_dir = data_dir / "topics";
  std::error_code ec;
  if (!std::filesystem::exists(topics_dir, ec)) {
    return summary;
  }

  for (const auto& entry : std::filesystem::directory_iterator(topics_dir)) {
    if (!entry.is_directory()) {
      continue;
    }

    const auto topic = entry.path().filename().string();
    if (!is_valid_topic_name(topic)) {
      continue;
    }

    bool topic_counted = false;
    for (const auto& partition_entry : std::filesystem::directory_iterator(entry.path())) {
      if (!partition_entry.is_directory()) {
        continue;
      }
      std::uint16_t partition_id = 0;
      if (!parse_partition_id(partition_entry.path(), partition_id)) {
        continue;
      }

      auto log = PartitionLog::open({data_dir, topic, partition_id, max_segment_bytes});
      const auto& stats = log.recovery_stats();
      if (!topic_counted) {
        ++summary.topics_recovered;
        topic_counted = true;
      }
      ++summary.partitions_recovered;
      summary.segments_scanned += stats.segments_scanned;
      summary.indexes_rebuilt += stats.indexes_rebuilt;
      summary.records_recovered += stats.records_recovered;
      summary.bytes_truncated += stats.bytes_truncated;
    }
  }
  return summary;
}

} // namespace boltstream::storage
