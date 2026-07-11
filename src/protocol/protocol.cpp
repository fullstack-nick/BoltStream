#include "boltstream/protocol/protocol.h"

#include <array>
#include <limits>
#include <utility>

namespace boltstream::protocol {
namespace {

constexpr std::size_t kHeaderCrcOffset = 28;
constexpr std::uint32_t kCompressedFetchMarker = 0x42544348U;

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
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8U) | static_cast<std::uint64_t>(bytes[offset + index]);
  }
  return value;
}

void write_string(std::vector<std::uint8_t>& out, std::string_view value) {
  write_u32(out, static_cast<std::uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

void write_bytes(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> value) {
  write_u32(out, static_cast<std::uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

class PayloadReader {
public:
  explicit PayloadReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  bool read_u16(std::uint16_t& value) {
    if (remaining() < sizeof(std::uint16_t)) {
      return false;
    }
    value = read_u16_be(bytes_, offset_);
    offset_ += sizeof(std::uint16_t);
    return true;
  }

  bool read_u32(std::uint32_t& value) {
    if (remaining() < sizeof(std::uint32_t)) {
      return false;
    }
    value = read_u32_be(bytes_, offset_);
    offset_ += sizeof(std::uint32_t);
    return true;
  }

  bool read_u64(std::uint64_t& value) {
    if (remaining() < sizeof(std::uint64_t)) {
      return false;
    }
    value = read_u64_be(bytes_, offset_);
    offset_ += sizeof(std::uint64_t);
    return true;
  }

  bool read_string(std::string& value) {
    std::uint32_t size = 0;
    if (!read_u32(size) || remaining() < size) {
      return false;
    }
    value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }

  bool read_bytes(std::vector<std::uint8_t>& value) {
    std::uint32_t size = 0;
    if (!read_u32(size) || remaining() < size) {
      return false;
    }
    value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                 bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += size;
    return true;
  }

  [[nodiscard]] bool done() const { return offset_ == bytes_.size(); }
  [[nodiscard]] std::size_t remaining() const { return bytes_.size() - offset_; }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t offset_{0};
};

DecodeResult ok_result() {
  DecodeResult result;
  result.ok = true;
  return result;
}

DecodeResult error_result(ErrorCode code, std::string message) {
  DecodeResult result;
  result.ok = false;
  result.error = code;
  result.message = std::move(message);
  return result;
}

void write_bool(std::vector<std::uint8_t>& out, bool value) { write_u32(out, value ? 1U : 0U); }

bool read_bool(PayloadReader& reader, bool& value) {
  std::uint32_t encoded = 0;
  if (!reader.read_u32(encoded) || encoded > 1U) {
    return false;
  }
  value = encoded == 1U;
  return true;
}

void write_topic_partition_description(std::vector<std::uint8_t>& out,
                                       const TopicPartitionDescription& partition) {
  write_u16(out, partition.partition);
  write_u64(out, partition.earliest_offset);
  write_u64(out, partition.next_offset);
  write_u32(out, partition.segment_count);
  write_u64(out, partition.log_bytes);
}

bool read_topic_partition_description(PayloadReader& reader, TopicPartitionDescription& partition) {
  return reader.read_u16(partition.partition) && reader.read_u64(partition.earliest_offset) &&
         reader.read_u64(partition.next_offset) && reader.read_u32(partition.segment_count) &&
         reader.read_u64(partition.log_bytes);
}

void write_topic_description(std::vector<std::uint8_t>& out, const TopicDescription& topic) {
  write_string(out, topic.topic);
  write_u16(out, topic.partition_count);
  write_u64(out, topic.log_bytes);
  write_u32(out, static_cast<std::uint32_t>(topic.partitions.size()));
  for (const auto& partition : topic.partitions) {
    write_topic_partition_description(out, partition);
  }
}

bool read_topic_description(PayloadReader& reader, TopicDescription& topic) {
  std::uint32_t partition_count = 0;
  if (!reader.read_string(topic.topic) || !reader.read_u16(topic.partition_count) ||
      !reader.read_u64(topic.log_bytes) || !reader.read_u32(partition_count)) {
    return false;
  }
  topic.partitions.clear();
  topic.partitions.reserve(partition_count);
  for (std::uint32_t index = 0; index < partition_count; ++index) {
    TopicPartitionDescription partition;
    if (!read_topic_partition_description(reader, partition)) {
      return false;
    }
    topic.partitions.push_back(partition);
  }
  return true;
}

} // namespace

std::string_view frame_type_name(FrameType frame_type) {
  switch (frame_type) {
  case FrameType::ErrorResponse:
    return "error_response";
  case FrameType::HealthRequest:
    return "health_request";
  case FrameType::HealthResponse:
    return "health_response";
  case FrameType::MetadataRequest:
    return "metadata_request";
  case FrameType::MetadataResponse:
    return "metadata_response";
  case FrameType::ProduceRequest:
    return "produce_request";
  case FrameType::ProduceResponse:
    return "produce_response";
  case FrameType::FetchRequest:
    return "fetch_request";
  case FrameType::FetchResponse:
    return "fetch_response";
  case FrameType::OffsetCommitRequest:
    return "offset_commit_request";
  case FrameType::OffsetCommitResponse:
    return "offset_commit_response";
  case FrameType::AuthRequest:
    return "auth_request";
  case FrameType::AuthResponse:
    return "auth_response";
  case FrameType::CreateTopicRequest:
    return "create_topic_request";
  case FrameType::CreateTopicResponse:
    return "create_topic_response";
  case FrameType::JoinGroupRequest:
    return "join_group_request";
  case FrameType::JoinGroupResponse:
    return "join_group_response";
  case FrameType::SyncGroupRequest:
    return "sync_group_request";
  case FrameType::SyncGroupResponse:
    return "sync_group_response";
  case FrameType::HeartbeatRequest:
    return "heartbeat_request";
  case FrameType::HeartbeatResponse:
    return "heartbeat_response";
  case FrameType::LeaveGroupRequest:
    return "leave_group_request";
  case FrameType::LeaveGroupResponse:
    return "leave_group_response";
  case FrameType::GroupOffsetCommitRequest:
    return "group_offset_commit_request";
  case FrameType::GroupOffsetCommitResponse:
    return "group_offset_commit_response";
  case FrameType::ListTopicsRequest:
    return "list_topics_request";
  case FrameType::ListTopicsResponse:
    return "list_topics_response";
  case FrameType::DescribeTopicRequest:
    return "describe_topic_request";
  case FrameType::DescribeTopicResponse:
    return "describe_topic_response";
  case FrameType::DeleteTopicRequest:
    return "delete_topic_request";
  case FrameType::DeleteTopicResponse:
    return "delete_topic_response";
  case FrameType::RunRetentionRequest:
    return "run_retention_request";
  case FrameType::RunRetentionResponse:
    return "run_retention_response";
  case FrameType::DescribeGroupRequest:
    return "describe_group_request";
  case FrameType::DescribeGroupResponse:
    return "describe_group_response";
  case FrameType::ResetGroupOffsetRequest:
    return "reset_group_offset_request";
  case FrameType::ResetGroupOffsetResponse:
    return "reset_group_offset_response";
  case FrameType::ProduceBatchRequest:
    return "produce_batch_request";
  case FrameType::ProduceBatchResponse:
    return "produce_batch_response";
  }
  return "unknown";
}

std::string_view error_code_name(ErrorCode error_code) {
  switch (error_code) {
  case ErrorCode::InvalidMagic:
    return "invalid_magic";
  case ErrorCode::UnsupportedVersion:
    return "unsupported_version";
  case ErrorCode::InvalidLength:
    return "invalid_length";
  case ErrorCode::CrcMismatch:
    return "crc_mismatch";
  case ErrorCode::MalformedPayload:
    return "malformed_payload";
  case ErrorCode::UnsupportedRequest:
    return "unsupported_request";
  case ErrorCode::NotImplemented:
    return "not_implemented";
  case ErrorCode::InternalError:
    return "internal_error";
  case ErrorCode::ReservedFlags:
    return "reserved_flags";
  case ErrorCode::Unauthorized:
    return "unauthorized";
  case ErrorCode::UnknownTopic:
    return "unknown_topic";
  case ErrorCode::TopicConflict:
    return "topic_conflict";
  case ErrorCode::InvalidPartition:
    return "invalid_partition";
  case ErrorCode::InvalidGroup:
    return "invalid_group";
  case ErrorCode::InvalidOffset:
    return "invalid_offset";
  case ErrorCode::Overloaded:
    return "overloaded";
  case ErrorCode::RebalanceRequired:
    return "rebalance_required";
  case ErrorCode::StaleMember:
    return "stale_member";
  case ErrorCode::OffsetOutOfRange:
    return "offset_out_of_range";
  case ErrorCode::GroupActive:
    return "group_active";
  case ErrorCode::UnsupportedCodec:
    return "unsupported_codec";
  case ErrorCode::InvalidBatch:
    return "invalid_batch";
  }
  return "unknown_error";
}

bool is_retryable_error(ErrorCode error_code) {
  return error_code == ErrorCode::Overloaded || error_code == ErrorCode::RebalanceRequired;
}

bool is_request_type(FrameType frame_type) {
  switch (frame_type) {
  case FrameType::HealthRequest:
  case FrameType::MetadataRequest:
  case FrameType::ProduceRequest:
  case FrameType::FetchRequest:
  case FrameType::OffsetCommitRequest:
  case FrameType::AuthRequest:
  case FrameType::CreateTopicRequest:
  case FrameType::JoinGroupRequest:
  case FrameType::SyncGroupRequest:
  case FrameType::HeartbeatRequest:
  case FrameType::LeaveGroupRequest:
  case FrameType::GroupOffsetCommitRequest:
  case FrameType::ListTopicsRequest:
  case FrameType::DescribeTopicRequest:
  case FrameType::DeleteTopicRequest:
  case FrameType::RunRetentionRequest:
  case FrameType::DescribeGroupRequest:
  case FrameType::ResetGroupOffsetRequest:
  case FrameType::ProduceBatchRequest:
    return true;
  default:
    return false;
  }
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const auto byte : bytes) {
    crc ^= static_cast<std::uint32_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      const auto mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

std::vector<std::uint8_t> encode_frame(FrameType frame_type, std::uint64_t correlation_id,
                                       std::span<const std::uint8_t> payload, std::uint32_t flags) {
  return encode_frame(kProtocolVersion, frame_type, correlation_id, payload, flags);
}

std::vector<std::uint8_t> encode_frame(std::uint16_t version, FrameType frame_type,
                                       std::uint64_t correlation_id,
                                       std::span<const std::uint8_t> payload, std::uint32_t flags) {
  std::vector<std::uint8_t> out;
  out.reserve(kFrameHeaderBytes + payload.size());

  write_u32(out, kMagic);
  write_u16(out, version);
  write_u16(out, static_cast<std::uint16_t>(frame_type));
  write_u32(out, kFrameHeaderBytes);
  write_u32(out, static_cast<std::uint32_t>(payload.size()));
  write_u64(out, correlation_id);
  write_u32(out, flags);

  const auto header_crc = crc32(std::span<const std::uint8_t>{out.data(), out.size()});
  write_u32(out, header_crc);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

HeaderDecodeResult decode_header(std::span<const std::uint8_t> bytes,
                                 std::uint32_t max_frame_bytes) {
  HeaderDecodeResult result;
  if (bytes.size() < kFrameHeaderBytes) {
    result.error = ErrorCode::InvalidLength;
    result.message = "truncated frame header";
    return result;
  }

  result.header.magic = read_u32_be(bytes, 0);
  result.header.version = read_u16_be(bytes, 4);
  result.header.frame_type = static_cast<FrameType>(read_u16_be(bytes, 6));
  result.header.header_bytes = read_u32_be(bytes, 8);
  result.header.payload_bytes = read_u32_be(bytes, 12);
  result.header.correlation_id = read_u64_be(bytes, 16);
  result.header.flags = read_u32_be(bytes, 24);
  result.header.header_crc32 = read_u32_be(bytes, 28);

  if (result.header.magic != kMagic) {
    result.error = ErrorCode::InvalidMagic;
    result.message = "invalid frame magic";
    return result;
  }
  if (result.header.version < kMinimumProtocolVersion || result.header.version > kProtocolVersion) {
    result.error = ErrorCode::UnsupportedVersion;
    result.message = "unsupported protocol version";
    return result;
  }
  if (result.header.header_bytes != kFrameHeaderBytes) {
    result.error = ErrorCode::InvalidLength;
    result.message = "unsupported frame header length";
    return result;
  }
  if (max_frame_bytes < kFrameHeaderBytes ||
      result.header.payload_bytes > max_frame_bytes - kFrameHeaderBytes) {
    result.error = ErrorCode::InvalidLength;
    result.message = "frame exceeds configured maximum";
    return result;
  }
  if (result.header.flags != 0) {
    result.error = ErrorCode::ReservedFlags;
    result.message = "reserved frame flags are not supported";
    return result;
  }

  const auto expected_crc = crc32(std::span<const std::uint8_t>{bytes.data(), kHeaderCrcOffset});
  if (expected_crc != result.header.header_crc32) {
    result.error = ErrorCode::CrcMismatch;
    result.message = "frame header crc mismatch";
    return result;
  }

  result.ok = true;
  return result;
}

FrameDecodeResult decode_frame(std::span<const std::uint8_t> bytes, std::uint32_t max_frame_bytes) {
  FrameDecodeResult result;
  const auto header = decode_header(bytes, max_frame_bytes);
  if (!header.ok) {
    result.error = header.error;
    result.message = header.message;
    return result;
  }

  const auto expected_size = static_cast<std::size_t>(kFrameHeaderBytes) +
                             static_cast<std::size_t>(header.header.payload_bytes);
  if (bytes.size() != expected_size) {
    result.error = ErrorCode::InvalidLength;
    result.message =
        bytes.size() < expected_size ? "truncated frame payload" : "frame contains trailing bytes";
    return result;
  }

  result.frame.header = header.header;
  result.frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes),
                              bytes.end());
  result.ok = true;
  return result;
}

std::vector<std::uint8_t> encode_error_response(ErrorCode code, std::string_view message) {
  std::vector<std::uint8_t> out;
  write_u32(out, static_cast<std::uint32_t>(code));
  write_string(out, message);
  return out;
}

DecodeResult decode_error_response(std::span<const std::uint8_t> payload, ErrorResponse& response) {
  PayloadReader reader{payload};
  std::uint32_t code = 0;
  if (!reader.read_u32(code) || !reader.read_string(response.message) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed error response payload");
  }
  response.code = static_cast<ErrorCode>(code);
  return ok_result();
}

std::vector<std::uint8_t> encode_health_response(std::string_view status, std::string_view detail) {
  std::vector<std::uint8_t> out;
  write_string(out, status);
  write_string(out, detail);
  return out;
}

DecodeResult decode_health_response(std::span<const std::uint8_t> payload,
                                    HealthResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.status) || !reader.read_string(response.detail) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed health response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_auth_request(std::string_view token) {
  std::vector<std::uint8_t> out;
  write_string(out, token);
  return out;
}

DecodeResult decode_auth_request(std::span<const std::uint8_t> payload, AuthRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.token) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed auth request payload");
  }
  if (request.token.empty()) {
    return error_result(ErrorCode::MalformedPayload, "auth token must not be empty");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_auth_response(std::string_view status) {
  std::vector<std::uint8_t> out;
  write_string(out, status);
  return out;
}

DecodeResult decode_auth_response(std::span<const std::uint8_t> payload, AuthResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.status) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed auth response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_create_topic_request(std::string_view topic,
                                                      std::uint16_t partition_count) {
  std::vector<std::uint8_t> out;
  write_string(out, topic);
  write_u16(out, partition_count);
  return out;
}

DecodeResult decode_create_topic_request(std::span<const std::uint8_t> payload,
                                         CreateTopicRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.topic) || !reader.read_u16(request.partition_count) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed create-topic request payload");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "create-topic topic must not be empty");
  }
  if (request.partition_count == 0) {
    return error_result(ErrorCode::MalformedPayload,
                        "create-topic partition count must be greater than zero");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_create_topic_response(const CreateTopicResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.topic);
  write_u16(out, response.partition_count);
  write_string(out, response.status);
  return out;
}

DecodeResult decode_create_topic_response(std::span<const std::uint8_t> payload,
                                          CreateTopicResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.topic) || !reader.read_u16(response.partition_count) ||
      !reader.read_string(response.status) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed create-topic response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_produce_request(std::string_view topic,
                                                 std::span<const std::uint8_t> key,
                                                 std::span<const std::uint8_t> message) {
  std::vector<std::uint8_t> out;
  write_string(out, topic);
  write_bytes(out, key);
  write_bytes(out, message);
  return out;
}

DecodeResult decode_produce_request(std::span<const std::uint8_t> payload,
                                    ProduceRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.topic) || !reader.read_bytes(request.key) ||
      !reader.read_bytes(request.message) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed produce request payload");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "produce topic must not be empty");
  }
  if (request.message.empty()) {
    return error_result(ErrorCode::MalformedPayload, "produce message must not be empty");
  }
  return ok_result();
}

DecodeResult validate_produce_request(std::span<const std::uint8_t> payload) {
  ProduceRequest request;
  return decode_produce_request(payload, request);
}

std::vector<std::uint8_t> encode_record_set(std::span<const BatchRecord> records) {
  std::vector<std::uint8_t> out;
  write_u32(out, static_cast<std::uint32_t>(records.size()));
  for (const auto& record : records) {
    write_bytes(out, record.key);
    write_bytes(out, record.message);
  }
  return out;
}

DecodeResult decode_record_set(std::span<const std::uint8_t> payload, std::uint32_t expected_count,
                               std::vector<BatchRecord>& records) {
  PayloadReader reader{payload};
  std::uint32_t count = 0;
  if (!reader.read_u32(count) || count != expected_count) {
    return error_result(ErrorCode::InvalidBatch, "record-set count mismatch");
  }
  records.clear();
  records.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    BatchRecord record;
    if (!reader.read_bytes(record.key) || !reader.read_bytes(record.message) ||
        record.message.empty()) {
      return error_result(ErrorCode::InvalidBatch, "malformed record set");
    }
    records.push_back(std::move(record));
  }
  if (!reader.done()) {
    return error_result(ErrorCode::InvalidBatch, "trailing record-set bytes");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_produce_batch_request(const ProduceBatchRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.topic);
  write_u16(out, request.partition);
  write_u16(out, static_cast<std::uint16_t>(request.codec));
  write_u32(out, request.record_count);
  write_u32(out, request.uncompressed_bytes);
  write_bytes(out, request.encoded_records);
  return out;
}

DecodeResult decode_produce_batch_request(std::span<const std::uint8_t> payload,
                                          ProduceBatchRequest& request) {
  PayloadReader reader{payload};
  std::uint16_t codec = 0;
  if (!reader.read_string(request.topic) || !reader.read_u16(request.partition) ||
      !reader.read_u16(codec) || !reader.read_u32(request.record_count) ||
      !reader.read_u32(request.uncompressed_bytes) || !reader.read_bytes(request.encoded_records) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed produce-batch request");
  }
  request.codec = static_cast<compression::Codec>(codec);
  if (request.topic.empty() || request.record_count == 0 || request.uncompressed_bytes == 0 ||
      request.encoded_records.empty()) {
    return error_result(ErrorCode::InvalidBatch, "produce batch must not be empty");
  }
  if (!compression::is_supported(request.codec)) {
    return error_result(ErrorCode::UnsupportedCodec, "unsupported compression codec");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_produce_batch_response(const ProduceBatchResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.topic);
  write_u16(out, response.partition);
  write_u64(out, response.base_offset);
  write_u64(out, response.next_offset);
  write_u32(out, response.record_count);
  write_u32(out, response.logical_bytes);
  write_u32(out, response.encoded_bytes);
  write_u32(out, response.stored_bytes);
  return out;
}

DecodeResult decode_produce_batch_response(std::span<const std::uint8_t> payload,
                                           ProduceBatchResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.topic) || !reader.read_u16(response.partition) ||
      !reader.read_u64(response.base_offset) || !reader.read_u64(response.next_offset) ||
      !reader.read_u32(response.record_count) || !reader.read_u32(response.logical_bytes) ||
      !reader.read_u32(response.encoded_bytes) || !reader.read_u32(response.stored_bytes) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed produce-batch response");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_fetch_request(std::string_view topic, std::uint16_t partition,
                                               std::string_view from, std::string_view group,
                                               std::uint32_t max_wait_ms) {
  std::vector<std::uint8_t> out;
  write_string(out, topic);
  write_u16(out, partition);
  write_string(out, from);
  write_string(out, group);
  write_u32(out, max_wait_ms);
  return out;
}

DecodeResult decode_fetch_request(std::span<const std::uint8_t> payload, FetchRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.topic) || !reader.read_u16(request.partition) ||
      !reader.read_string(request.from) || !reader.read_string(request.group) ||
      !reader.read_u32(request.max_wait_ms) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed fetch request payload");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "fetch topic must not be empty");
  }
  if (request.from.empty()) {
    return error_result(ErrorCode::MalformedPayload, "fetch offset selector must not be empty");
  }
  return ok_result();
}

DecodeResult validate_fetch_request(std::span<const std::uint8_t> payload) {
  FetchRequest request;
  return decode_fetch_request(payload, request);
}

std::vector<std::uint8_t> encode_offset_commit_request(const OffsetCommitRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.group);
  write_string(out, request.topic);
  write_u16(out, request.partition);
  write_u64(out, request.next_offset);
  return out;
}

DecodeResult decode_offset_commit_request(std::span<const std::uint8_t> payload,
                                          OffsetCommitRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.group) || !reader.read_string(request.topic) ||
      !reader.read_u16(request.partition) || !reader.read_u64(request.next_offset) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed offset-commit request payload");
  }
  if (request.group.empty()) {
    return error_result(ErrorCode::MalformedPayload, "offset-commit group must not be empty");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "offset-commit topic must not be empty");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_offset_commit_response(const OffsetCommitResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.group);
  write_string(out, response.topic);
  write_u16(out, response.partition);
  write_u64(out, response.next_offset);
  return out;
}

DecodeResult decode_offset_commit_response(std::span<const std::uint8_t> payload,
                                           OffsetCommitResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.group) || !reader.read_string(response.topic) ||
      !reader.read_u16(response.partition) || !reader.read_u64(response.next_offset) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed offset-commit response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_join_group_request(const JoinGroupRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.group);
  write_string(out, request.topic);
  write_string(out, request.member_id);
  write_u32(out, request.session_timeout_ms);
  return out;
}

DecodeResult decode_join_group_request(std::span<const std::uint8_t> payload,
                                       JoinGroupRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.group) || !reader.read_string(request.topic) ||
      !reader.read_string(request.member_id) || !reader.read_u32(request.session_timeout_ms) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed join-group request payload");
  }
  if (request.group.empty()) {
    return error_result(ErrorCode::MalformedPayload, "join-group group must not be empty");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "join-group topic must not be empty");
  }
  if (request.session_timeout_ms == 0) {
    return error_result(ErrorCode::MalformedPayload,
                        "join-group session timeout must be greater than zero");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_join_group_response(const JoinGroupResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.group);
  write_string(out, response.topic);
  write_string(out, response.member_id);
  write_u64(out, response.generation_id);
  return out;
}

DecodeResult decode_join_group_response(std::span<const std::uint8_t> payload,
                                        JoinGroupResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.group) || !reader.read_string(response.topic) ||
      !reader.read_string(response.member_id) || !reader.read_u64(response.generation_id) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed join-group response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_sync_group_request(const SyncGroupRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.group);
  write_string(out, request.topic);
  write_string(out, request.member_id);
  write_u64(out, request.generation_id);
  return out;
}

DecodeResult decode_sync_group_request(std::span<const std::uint8_t> payload,
                                       SyncGroupRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.group) || !reader.read_string(request.topic) ||
      !reader.read_string(request.member_id) || !reader.read_u64(request.generation_id) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed sync-group request payload");
  }
  if (request.group.empty()) {
    return error_result(ErrorCode::MalformedPayload, "sync-group group must not be empty");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "sync-group topic must not be empty");
  }
  if (request.member_id.empty()) {
    return error_result(ErrorCode::MalformedPayload, "sync-group member id must not be empty");
  }
  if (request.generation_id == 0) {
    return error_result(ErrorCode::MalformedPayload,
                        "sync-group generation id must be greater than zero");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_sync_group_response(const SyncGroupResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.group);
  write_string(out, response.topic);
  write_string(out, response.member_id);
  write_u64(out, response.generation_id);
  write_u32(out, static_cast<std::uint32_t>(response.assignment.size()));
  for (const auto partition : response.assignment) {
    write_u16(out, partition);
  }
  return out;
}

DecodeResult decode_sync_group_response(std::span<const std::uint8_t> payload,
                                        SyncGroupResponse& response) {
  PayloadReader reader{payload};
  std::uint32_t assignment_count = 0;
  if (!reader.read_string(response.group) || !reader.read_string(response.topic) ||
      !reader.read_string(response.member_id) || !reader.read_u64(response.generation_id) ||
      !reader.read_u32(assignment_count)) {
    return error_result(ErrorCode::MalformedPayload, "malformed sync-group response payload");
  }
  response.assignment.clear();
  response.assignment.reserve(assignment_count);
  for (std::uint32_t index = 0; index < assignment_count; ++index) {
    std::uint16_t partition = 0;
    if (!reader.read_u16(partition)) {
      return error_result(ErrorCode::MalformedPayload, "malformed sync-group response payload");
    }
    response.assignment.push_back(partition);
  }
  if (!reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed sync-group response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_heartbeat_request(const HeartbeatRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.group);
  write_string(out, request.topic);
  write_string(out, request.member_id);
  write_u64(out, request.generation_id);
  return out;
}

DecodeResult decode_heartbeat_request(std::span<const std::uint8_t> payload,
                                      HeartbeatRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.group) || !reader.read_string(request.topic) ||
      !reader.read_string(request.member_id) || !reader.read_u64(request.generation_id) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed heartbeat request payload");
  }
  if (request.group.empty()) {
    return error_result(ErrorCode::MalformedPayload, "heartbeat group must not be empty");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "heartbeat topic must not be empty");
  }
  if (request.member_id.empty()) {
    return error_result(ErrorCode::MalformedPayload, "heartbeat member id must not be empty");
  }
  if (request.generation_id == 0) {
    return error_result(ErrorCode::MalformedPayload,
                        "heartbeat generation id must be greater than zero");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_heartbeat_response(const HeartbeatResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.group);
  write_string(out, response.topic);
  write_string(out, response.member_id);
  write_u64(out, response.generation_id);
  write_string(out, response.status);
  return out;
}

DecodeResult decode_heartbeat_response(std::span<const std::uint8_t> payload,
                                       HeartbeatResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.group) || !reader.read_string(response.topic) ||
      !reader.read_string(response.member_id) || !reader.read_u64(response.generation_id) ||
      !reader.read_string(response.status) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed heartbeat response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_leave_group_request(const LeaveGroupRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.group);
  write_string(out, request.topic);
  write_string(out, request.member_id);
  write_u64(out, request.generation_id);
  return out;
}

DecodeResult decode_leave_group_request(std::span<const std::uint8_t> payload,
                                        LeaveGroupRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.group) || !reader.read_string(request.topic) ||
      !reader.read_string(request.member_id) || !reader.read_u64(request.generation_id) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed leave-group request payload");
  }
  if (request.group.empty()) {
    return error_result(ErrorCode::MalformedPayload, "leave-group group must not be empty");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "leave-group topic must not be empty");
  }
  if (request.member_id.empty()) {
    return error_result(ErrorCode::MalformedPayload, "leave-group member id must not be empty");
  }
  if (request.generation_id == 0) {
    return error_result(ErrorCode::MalformedPayload,
                        "leave-group generation id must be greater than zero");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_leave_group_response(const LeaveGroupResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.group);
  write_string(out, response.topic);
  write_string(out, response.member_id);
  write_u64(out, response.generation_id);
  write_string(out, response.status);
  return out;
}

DecodeResult decode_leave_group_response(std::span<const std::uint8_t> payload,
                                         LeaveGroupResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.group) || !reader.read_string(response.topic) ||
      !reader.read_string(response.member_id) || !reader.read_u64(response.generation_id) ||
      !reader.read_string(response.status) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed leave-group response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t>
encode_group_offset_commit_request(const GroupOffsetCommitRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.group);
  write_string(out, request.topic);
  write_string(out, request.member_id);
  write_u64(out, request.generation_id);
  write_u16(out, request.partition);
  write_u64(out, request.next_offset);
  return out;
}

DecodeResult decode_group_offset_commit_request(std::span<const std::uint8_t> payload,
                                                GroupOffsetCommitRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.group) || !reader.read_string(request.topic) ||
      !reader.read_string(request.member_id) || !reader.read_u64(request.generation_id) ||
      !reader.read_u16(request.partition) || !reader.read_u64(request.next_offset) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload,
                        "malformed group-offset-commit request payload");
  }
  if (request.group.empty()) {
    return error_result(ErrorCode::MalformedPayload, "group-offset-commit group must not be empty");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "group-offset-commit topic must not be empty");
  }
  if (request.member_id.empty()) {
    return error_result(ErrorCode::MalformedPayload,
                        "group-offset-commit member id must not be empty");
  }
  if (request.generation_id == 0) {
    return error_result(ErrorCode::MalformedPayload,
                        "group-offset-commit generation id must be greater than zero");
  }
  return ok_result();
}

std::vector<std::uint8_t>
encode_group_offset_commit_response(const GroupOffsetCommitResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.group);
  write_string(out, response.topic);
  write_string(out, response.member_id);
  write_u64(out, response.generation_id);
  write_u16(out, response.partition);
  write_u64(out, response.next_offset);
  return out;
}

DecodeResult decode_group_offset_commit_response(std::span<const std::uint8_t> payload,
                                                 GroupOffsetCommitResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.group) || !reader.read_string(response.topic) ||
      !reader.read_string(response.member_id) || !reader.read_u64(response.generation_id) ||
      !reader.read_u16(response.partition) || !reader.read_u64(response.next_offset) ||
      !reader.done()) {
    return error_result(ErrorCode::MalformedPayload,
                        "malformed group-offset-commit response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_list_topics_response(const ListTopicsResponse& response) {
  std::vector<std::uint8_t> out;
  write_u32(out, static_cast<std::uint32_t>(response.topics.size()));
  for (const auto& topic : response.topics) {
    write_topic_description(out, topic);
  }
  return out;
}

DecodeResult decode_list_topics_response(std::span<const std::uint8_t> payload,
                                         ListTopicsResponse& response) {
  PayloadReader reader{payload};
  std::uint32_t topic_count = 0;
  if (!reader.read_u32(topic_count)) {
    return error_result(ErrorCode::MalformedPayload, "malformed list-topics response payload");
  }
  response.topics.clear();
  response.topics.reserve(topic_count);
  for (std::uint32_t index = 0; index < topic_count; ++index) {
    TopicDescription topic;
    if (!read_topic_description(reader, topic)) {
      return error_result(ErrorCode::MalformedPayload, "malformed list-topics response payload");
    }
    response.topics.push_back(std::move(topic));
  }
  if (!reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed list-topics response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_describe_topic_request(const DescribeTopicRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.topic);
  return out;
}

DecodeResult decode_describe_topic_request(std::span<const std::uint8_t> payload,
                                           DescribeTopicRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.topic) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed describe-topic request payload");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "describe-topic topic must not be empty");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_describe_topic_response(const DescribeTopicResponse& response) {
  std::vector<std::uint8_t> out;
  write_topic_description(out, response.topic);
  return out;
}

DecodeResult decode_describe_topic_response(std::span<const std::uint8_t> payload,
                                            DescribeTopicResponse& response) {
  PayloadReader reader{payload};
  if (!read_topic_description(reader, response.topic) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed describe-topic response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_delete_topic_request(const DeleteTopicRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.topic);
  return out;
}

DecodeResult decode_delete_topic_request(std::span<const std::uint8_t> payload,
                                         DeleteTopicRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.topic) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed delete-topic request payload");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "delete-topic topic must not be empty");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_delete_topic_response(const DeleteTopicResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.topic);
  write_string(out, response.status);
  write_u16(out, response.partitions_deleted);
  write_u32(out, response.segments_deleted);
  write_u64(out, response.bytes_deleted);
  write_u32(out, response.offsets_removed);
  return out;
}

DecodeResult decode_delete_topic_response(std::span<const std::uint8_t> payload,
                                          DeleteTopicResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.topic) || !reader.read_string(response.status) ||
      !reader.read_u16(response.partitions_deleted) ||
      !reader.read_u32(response.segments_deleted) || !reader.read_u64(response.bytes_deleted) ||
      !reader.read_u32(response.offsets_removed) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed delete-topic response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_run_retention_request(const RunRetentionRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.topic);
  return out;
}

DecodeResult decode_run_retention_request(std::span<const std::uint8_t> payload,
                                          RunRetentionRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.topic) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed run-retention request payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_run_retention_response(const RunRetentionResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.topic);
  write_u32(out, response.topics_scanned);
  write_u32(out, response.partitions_scanned);
  write_u32(out, response.segments_deleted);
  write_u64(out, response.bytes_deleted);
  write_u32(out, static_cast<std::uint32_t>(response.partitions.size()));
  for (const auto& partition : response.partitions) {
    write_string(out, partition.topic);
    write_u16(out, partition.partition);
    write_u32(out, partition.segments_deleted);
    write_u64(out, partition.bytes_deleted);
    write_u64(out, partition.earliest_offset);
    write_u64(out, partition.next_offset);
  }
  return out;
}

DecodeResult decode_run_retention_response(std::span<const std::uint8_t> payload,
                                           RunRetentionResponse& response) {
  PayloadReader reader{payload};
  std::uint32_t partition_count = 0;
  if (!reader.read_string(response.topic) || !reader.read_u32(response.topics_scanned) ||
      !reader.read_u32(response.partitions_scanned) ||
      !reader.read_u32(response.segments_deleted) || !reader.read_u64(response.bytes_deleted) ||
      !reader.read_u32(partition_count)) {
    return error_result(ErrorCode::MalformedPayload, "malformed run-retention response payload");
  }
  response.partitions.clear();
  response.partitions.reserve(partition_count);
  for (std::uint32_t index = 0; index < partition_count; ++index) {
    RetentionPartitionResult partition;
    if (!reader.read_string(partition.topic) || !reader.read_u16(partition.partition) ||
        !reader.read_u32(partition.segments_deleted) || !reader.read_u64(partition.bytes_deleted) ||
        !reader.read_u64(partition.earliest_offset) || !reader.read_u64(partition.next_offset)) {
      return error_result(ErrorCode::MalformedPayload, "malformed run-retention response payload");
    }
    response.partitions.push_back(std::move(partition));
  }
  if (!reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed run-retention response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_describe_group_request(const DescribeGroupRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.group);
  write_string(out, request.topic);
  return out;
}

DecodeResult decode_describe_group_request(std::span<const std::uint8_t> payload,
                                           DescribeGroupRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.group) || !reader.read_string(request.topic) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed describe-group request payload");
  }
  if (request.group.empty()) {
    return error_result(ErrorCode::MalformedPayload, "describe-group group must not be empty");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "describe-group topic must not be empty");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_describe_group_response(const DescribeGroupResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.group);
  write_string(out, response.topic);
  write_u32(out, response.active_member_count);
  write_u32(out, static_cast<std::uint32_t>(response.offsets.size()));
  for (const auto& offset : response.offsets) {
    write_u16(out, offset.partition);
    write_bool(out, offset.has_committed_offset);
    write_u64(out, offset.committed_offset);
    write_u64(out, offset.earliest_offset);
    write_u64(out, offset.next_offset);
    write_u64(out, offset.lag);
    write_bool(out, offset.out_of_range);
  }
  return out;
}

DecodeResult decode_describe_group_response(std::span<const std::uint8_t> payload,
                                            DescribeGroupResponse& response) {
  PayloadReader reader{payload};
  std::uint32_t offset_count = 0;
  if (!reader.read_string(response.group) || !reader.read_string(response.topic) ||
      !reader.read_u32(response.active_member_count) || !reader.read_u32(offset_count)) {
    return error_result(ErrorCode::MalformedPayload, "malformed describe-group response payload");
  }
  response.offsets.clear();
  response.offsets.reserve(offset_count);
  for (std::uint32_t index = 0; index < offset_count; ++index) {
    GroupOffsetDescription offset;
    if (!reader.read_u16(offset.partition) || !read_bool(reader, offset.has_committed_offset) ||
        !reader.read_u64(offset.committed_offset) || !reader.read_u64(offset.earliest_offset) ||
        !reader.read_u64(offset.next_offset) || !reader.read_u64(offset.lag) ||
        !read_bool(reader, offset.out_of_range)) {
      return error_result(ErrorCode::MalformedPayload, "malformed describe-group response payload");
    }
    response.offsets.push_back(offset);
  }
  if (!reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed describe-group response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t>
encode_reset_group_offset_request(const ResetGroupOffsetRequest& request) {
  std::vector<std::uint8_t> out;
  write_string(out, request.group);
  write_string(out, request.topic);
  write_u16(out, request.partition);
  write_string(out, request.to);
  return out;
}

DecodeResult decode_reset_group_offset_request(std::span<const std::uint8_t> payload,
                                               ResetGroupOffsetRequest& request) {
  PayloadReader reader{payload};
  if (!reader.read_string(request.group) || !reader.read_string(request.topic) ||
      !reader.read_u16(request.partition) || !reader.read_string(request.to) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload,
                        "malformed reset-group-offset request payload");
  }
  if (request.group.empty()) {
    return error_result(ErrorCode::MalformedPayload, "reset-group-offset group must not be empty");
  }
  if (request.topic.empty()) {
    return error_result(ErrorCode::MalformedPayload, "reset-group-offset topic must not be empty");
  }
  if (request.to.empty()) {
    return error_result(ErrorCode::MalformedPayload, "reset-group-offset target must not be empty");
  }
  return ok_result();
}

std::vector<std::uint8_t>
encode_reset_group_offset_response(const ResetGroupOffsetResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.group);
  write_string(out, response.topic);
  write_u16(out, response.partition);
  write_u64(out, response.next_offset);
  write_string(out, response.status);
  return out;
}

DecodeResult decode_reset_group_offset_response(std::span<const std::uint8_t> payload,
                                                ResetGroupOffsetResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.group) || !reader.read_string(response.topic) ||
      !reader.read_u16(response.partition) || !reader.read_u64(response.next_offset) ||
      !reader.read_string(response.status) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload,
                        "malformed reset-group-offset response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_produce_response(const ProduceResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.topic);
  write_u16(out, response.partition);
  write_u64(out, response.offset);
  write_u64(out, response.next_offset);
  write_u32(out, response.encoded_byte_size);
  return out;
}

DecodeResult decode_produce_response(std::span<const std::uint8_t> payload,
                                     ProduceResponse& response) {
  PayloadReader reader{payload};
  if (!reader.read_string(response.topic) || !reader.read_u16(response.partition) ||
      !reader.read_u64(response.offset) || !reader.read_u64(response.next_offset) ||
      !reader.read_u32(response.encoded_byte_size) || !reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed produce response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_fetch_response(const FetchResponse& response) {
  std::vector<std::uint8_t> out;
  write_string(out, response.topic);
  write_u16(out, response.partition);
  write_u64(out, response.from_offset);
  write_u64(out, response.next_offset);
  write_u32(out, static_cast<std::uint32_t>(response.records.size()));
  for (const auto& record : response.records) {
    write_u64(out, record.offset);
    write_u64(out, record.timestamp_unix_ns);
    write_bytes(out, record.key);
    write_bytes(out, record.message);
    write_u32(out, record.encoded_byte_size);
  }
  return out;
}

std::vector<std::uint8_t>
encode_compressed_fetch_response(std::string_view topic, std::uint16_t partition,
                                 std::uint64_t from_offset, std::uint64_t next_offset,
                                 std::uint64_t timestamp_unix_ns, compression::Codec codec,
                                 std::uint32_t record_count, std::uint32_t uncompressed_bytes,
                                 std::span<const std::uint8_t> encoded_records) {
  std::vector<std::uint8_t> out;
  write_string(out, topic);
  write_u16(out, partition);
  write_u64(out, from_offset);
  write_u64(out, next_offset);
  write_u32(out, kCompressedFetchMarker);
  write_u16(out, static_cast<std::uint16_t>(codec));
  write_u64(out, timestamp_unix_ns);
  write_u32(out, record_count);
  write_u32(out, uncompressed_bytes);
  write_bytes(out, encoded_records);
  return out;
}

DecodeResult decode_fetch_response(std::span<const std::uint8_t> payload, FetchResponse& response) {
  PayloadReader reader{payload};
  std::uint32_t record_count = 0;
  if (!reader.read_string(response.topic) || !reader.read_u16(response.partition) ||
      !reader.read_u64(response.from_offset) || !reader.read_u64(response.next_offset) ||
      !reader.read_u32(record_count)) {
    return error_result(ErrorCode::MalformedPayload, "malformed fetch response payload");
  }

  response.records.clear();
  if (record_count == kCompressedFetchMarker) {
    std::uint16_t codec_value = 0;
    std::uint64_t timestamp = 0;
    std::uint32_t count = 0;
    std::uint32_t uncompressed = 0;
    std::vector<std::uint8_t> encoded;
    if (!reader.read_u16(codec_value) || !reader.read_u64(timestamp) || !reader.read_u32(count) ||
        !reader.read_u32(uncompressed) || !reader.read_bytes(encoded) || !reader.done()) {
      return error_result(ErrorCode::MalformedPayload, "malformed compressed fetch response");
    }
    const auto codec = static_cast<compression::Codec>(codec_value);
    try {
      const auto raw = compression::decompress(codec, encoded, uncompressed, kDefaultMaxFrameBytes);
      std::vector<BatchRecord> batch_records;
      const auto decoded = decode_record_set(raw, count, batch_records);
      if (!decoded.ok) {
        return decoded;
      }
      response.records.reserve(count);
      for (std::uint32_t index = 0; index < count; ++index) {
        response.records.push_back({response.from_offset + index, timestamp,
                                    std::move(batch_records[index].key),
                                    std::move(batch_records[index].message),
                                    static_cast<std::uint32_t>(encoded.size() / count)});
      }
      return ok_result();
    } catch (const std::exception& error) {
      return error_result(ErrorCode::InvalidBatch, error.what());
    }
  }
  response.records.reserve(record_count);
  for (std::uint32_t index = 0; index < record_count; ++index) {
    FetchRecord record;
    if (!reader.read_u64(record.offset) || !reader.read_u64(record.timestamp_unix_ns) ||
        !reader.read_bytes(record.key) || !reader.read_bytes(record.message) ||
        !reader.read_u32(record.encoded_byte_size)) {
      return error_result(ErrorCode::MalformedPayload, "malformed fetch response payload");
    }
    response.records.push_back(std::move(record));
  }

  if (!reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed fetch response payload");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_metadata_response(std::span<const MetadataTopic> topics) {
  std::vector<std::uint8_t> out;
  write_u32(out, static_cast<std::uint32_t>(topics.size()));
  for (const auto& topic : topics) {
    write_string(out, topic.topic);
    write_u16(out, topic.partition);
    write_u64(out, topic.next_offset);
  }
  return out;
}

std::vector<std::uint8_t> encode_metadata_request(std::uint32_t supported_codecs) {
  std::vector<std::uint8_t> out;
  write_u32(out, supported_codecs);
  return out;
}

DecodeResult decode_metadata_request(std::span<const std::uint8_t> payload,
                                     std::uint32_t& supported_codecs) {
  PayloadReader reader{payload};
  if (!reader.read_u32(supported_codecs) || !reader.done() ||
      (supported_codecs & compression::kNoneMask) == 0U) {
    return error_result(ErrorCode::MalformedPayload, "malformed metadata capabilities");
  }
  return ok_result();
}

std::vector<std::uint8_t> encode_metadata_response(const MetadataResponse& response,
                                                   std::uint16_t protocol_version) {
  if (protocol_version == 4) {
    return encode_metadata_response(response.topics);
  }
  std::vector<std::uint8_t> out;
  write_u32(out, response.supported_codecs);
  write_u32(out, response.negotiated_codecs);
  const auto topics = encode_metadata_response(response.topics);
  out.insert(out.end(), topics.begin(), topics.end());
  return out;
}

DecodeResult decode_metadata_response(std::span<const std::uint8_t> payload,
                                      MetadataResponse& response, std::uint16_t protocol_version) {
  if (protocol_version == 0) {
    protocol_version =
        payload.size() >= 8 && (read_u32_be(payload, 0) & compression::kNoneMask) != 0U &&
                (read_u32_be(payload, 0) & ~compression::kSupportedCodecMask) == 0U &&
                (read_u32_be(payload, 4) & compression::kNoneMask) != 0U &&
                (read_u32_be(payload, 4) & ~compression::kSupportedCodecMask) == 0U
            ? 5
            : 4;
  }
  PayloadReader reader{payload};
  std::uint32_t topic_count = 0;
  if (protocol_version >= 5 && (!reader.read_u32(response.supported_codecs) ||
                                !reader.read_u32(response.negotiated_codecs))) {
    return error_result(ErrorCode::MalformedPayload, "malformed metadata capabilities");
  }
  if (!reader.read_u32(topic_count)) {
    return error_result(ErrorCode::MalformedPayload, "malformed metadata response payload");
  }

  response.topics.clear();
  response.topics.reserve(topic_count);
  for (std::uint32_t index = 0; index < topic_count; ++index) {
    MetadataTopic topic;
    if (!reader.read_string(topic.topic) || !reader.read_u16(topic.partition) ||
        !reader.read_u64(topic.next_offset)) {
      return error_result(ErrorCode::MalformedPayload, "malformed metadata response payload");
    }
    response.topics.push_back(std::move(topic));
  }

  if (!reader.done()) {
    return error_result(ErrorCode::MalformedPayload, "malformed metadata response payload");
  }
  return ok_result();
}

DecodeResult validate_empty_payload(std::span<const std::uint8_t> payload) {
  if (!payload.empty()) {
    return error_result(ErrorCode::MalformedPayload, "request payload must be empty");
  }
  return ok_result();
}

} // namespace boltstream::protocol
