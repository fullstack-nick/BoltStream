#include "boltstream/protocol/protocol.h"

#include <array>
#include <limits>
#include <utility>

namespace boltstream::protocol {
namespace {

constexpr std::size_t kHeaderCrcOffset = 28;

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
  std::vector<std::uint8_t> out;
  out.reserve(kFrameHeaderBytes + payload.size());

  write_u32(out, kMagic);
  write_u16(out, kProtocolVersion);
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
  if (result.header.version != kProtocolVersion) {
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
    result.message = "reserved frame flags are not supported in protocol version 3";
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

DecodeResult decode_fetch_response(std::span<const std::uint8_t> payload, FetchResponse& response) {
  PayloadReader reader{payload};
  std::uint32_t record_count = 0;
  if (!reader.read_string(response.topic) || !reader.read_u16(response.partition) ||
      !reader.read_u64(response.from_offset) || !reader.read_u64(response.next_offset) ||
      !reader.read_u32(record_count)) {
    return error_result(ErrorCode::MalformedPayload, "malformed fetch response payload");
  }

  response.records.clear();
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

DecodeResult decode_metadata_response(std::span<const std::uint8_t> payload,
                                      MetadataResponse& response) {
  PayloadReader reader{payload};
  std::uint32_t topic_count = 0;
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
