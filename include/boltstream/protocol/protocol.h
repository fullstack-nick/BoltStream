#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boltstream::protocol {

inline constexpr std::uint32_t kMagic = 0x42535452U;
inline constexpr std::uint16_t kProtocolVersion = 3;
inline constexpr std::uint32_t kFrameHeaderBytes = 32;
inline constexpr std::uint32_t kDefaultMaxFrameBytes = 1024 * 1024;

enum class FrameType : std::uint16_t {
  ErrorResponse = 1,
  HealthRequest = 2,
  HealthResponse = 3,
  MetadataRequest = 4,
  MetadataResponse = 5,
  ProduceRequest = 6,
  ProduceResponse = 7,
  FetchRequest = 8,
  FetchResponse = 9,
  OffsetCommitRequest = 10,
  OffsetCommitResponse = 11,
  AuthRequest = 12,
  AuthResponse = 13,
  CreateTopicRequest = 14,
  CreateTopicResponse = 15,
  JoinGroupRequest = 16,
  JoinGroupResponse = 17,
  SyncGroupRequest = 18,
  SyncGroupResponse = 19,
  HeartbeatRequest = 20,
  HeartbeatResponse = 21,
  LeaveGroupRequest = 22,
  LeaveGroupResponse = 23,
  GroupOffsetCommitRequest = 24,
  GroupOffsetCommitResponse = 25,
};

enum class ErrorCode : std::uint32_t {
  InvalidMagic = 1,
  UnsupportedVersion = 2,
  InvalidLength = 3,
  CrcMismatch = 4,
  MalformedPayload = 5,
  UnsupportedRequest = 6,
  NotImplemented = 7,
  InternalError = 8,
  ReservedFlags = 9,
  Unauthorized = 10,
  UnknownTopic = 11,
  TopicConflict = 12,
  InvalidPartition = 13,
  InvalidGroup = 14,
  InvalidOffset = 15,
  Overloaded = 16,
  RebalanceRequired = 17,
  StaleMember = 18,
};

struct FrameHeader {
  std::uint32_t magic{kMagic};
  std::uint16_t version{kProtocolVersion};
  FrameType frame_type{FrameType::ErrorResponse};
  std::uint32_t header_bytes{kFrameHeaderBytes};
  std::uint32_t payload_bytes{0};
  std::uint64_t correlation_id{0};
  std::uint32_t flags{0};
  std::uint32_t header_crc32{0};
};

struct Frame {
  FrameHeader header;
  std::vector<std::uint8_t> payload;
};

struct DecodeResult {
  bool ok{false};
  ErrorCode error{ErrorCode::MalformedPayload};
  std::string message;
};

struct HeaderDecodeResult : DecodeResult {
  FrameHeader header;
};

struct FrameDecodeResult : DecodeResult {
  Frame frame;
};

struct ErrorResponse {
  ErrorCode code{ErrorCode::InternalError};
  std::string message;
};

struct HealthResponse {
  std::string status;
  std::string detail;
};

struct AuthRequest {
  std::string token;
};

struct AuthResponse {
  std::string status;
};

struct CreateTopicRequest {
  std::string topic;
  std::uint16_t partition_count{0};
};

struct CreateTopicResponse {
  std::string topic;
  std::uint16_t partition_count{0};
  std::string status;
};

struct ProduceRequest {
  std::string topic;
  std::vector<std::uint8_t> key;
  std::vector<std::uint8_t> message;
};

struct FetchRequest {
  std::string topic;
  std::uint16_t partition{0};
  std::string from;
  std::string group;
  std::uint32_t max_wait_ms{0};
};

struct OffsetCommitRequest {
  std::string group;
  std::string topic;
  std::uint16_t partition{0};
  std::uint64_t next_offset{0};
};

struct OffsetCommitResponse {
  std::string group;
  std::string topic;
  std::uint16_t partition{0};
  std::uint64_t next_offset{0};
};

struct JoinGroupRequest {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint32_t session_timeout_ms{0};
};

struct JoinGroupResponse {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint64_t generation_id{0};
};

struct SyncGroupRequest {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint64_t generation_id{0};
};

struct SyncGroupResponse {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint64_t generation_id{0};
  std::vector<std::uint16_t> assignment;
};

struct HeartbeatRequest {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint64_t generation_id{0};
};

struct HeartbeatResponse {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint64_t generation_id{0};
  std::string status;
};

struct LeaveGroupRequest {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint64_t generation_id{0};
};

struct LeaveGroupResponse {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint64_t generation_id{0};
  std::string status;
};

struct GroupOffsetCommitRequest {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint64_t generation_id{0};
  std::uint16_t partition{0};
  std::uint64_t next_offset{0};
};

struct GroupOffsetCommitResponse {
  std::string group;
  std::string topic;
  std::string member_id;
  std::uint64_t generation_id{0};
  std::uint16_t partition{0};
  std::uint64_t next_offset{0};
};

struct ProduceResponse {
  std::string topic;
  std::uint16_t partition{0};
  std::uint64_t offset{0};
  std::uint64_t next_offset{0};
  std::uint32_t encoded_byte_size{0};
};

struct FetchRecord {
  std::uint64_t offset{0};
  std::uint64_t timestamp_unix_ns{0};
  std::vector<std::uint8_t> key;
  std::vector<std::uint8_t> message;
  std::uint32_t encoded_byte_size{0};
};

struct FetchResponse {
  std::string topic;
  std::uint16_t partition{0};
  std::uint64_t from_offset{0};
  std::uint64_t next_offset{0};
  std::vector<FetchRecord> records;
};

struct MetadataTopic {
  std::string topic;
  std::uint16_t partition{0};
  std::uint64_t next_offset{0};
};

struct MetadataResponse {
  std::vector<MetadataTopic> topics;
};

std::string_view frame_type_name(FrameType frame_type);
std::string_view error_code_name(ErrorCode error_code);
bool is_retryable_error(ErrorCode error_code);
bool is_request_type(FrameType frame_type);

std::uint32_t crc32(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> encode_frame(FrameType frame_type, std::uint64_t correlation_id,
                                       std::span<const std::uint8_t> payload,
                                       std::uint32_t flags = 0);
HeaderDecodeResult decode_header(std::span<const std::uint8_t> bytes,
                                 std::uint32_t max_frame_bytes);
FrameDecodeResult decode_frame(std::span<const std::uint8_t> bytes, std::uint32_t max_frame_bytes);

std::vector<std::uint8_t> encode_error_response(ErrorCode code, std::string_view message);
DecodeResult decode_error_response(std::span<const std::uint8_t> payload, ErrorResponse& response);

std::vector<std::uint8_t> encode_health_response(std::string_view status, std::string_view detail);
DecodeResult decode_health_response(std::span<const std::uint8_t> payload,
                                    HealthResponse& response);

std::vector<std::uint8_t> encode_auth_request(std::string_view token);
DecodeResult decode_auth_request(std::span<const std::uint8_t> payload, AuthRequest& request);

std::vector<std::uint8_t> encode_auth_response(std::string_view status);
DecodeResult decode_auth_response(std::span<const std::uint8_t> payload, AuthResponse& response);

std::vector<std::uint8_t> encode_create_topic_request(std::string_view topic,
                                                      std::uint16_t partition_count);
DecodeResult decode_create_topic_request(std::span<const std::uint8_t> payload,
                                         CreateTopicRequest& request);
std::vector<std::uint8_t> encode_create_topic_response(const CreateTopicResponse& response);
DecodeResult decode_create_topic_response(std::span<const std::uint8_t> payload,
                                          CreateTopicResponse& response);

std::vector<std::uint8_t> encode_produce_request(std::string_view topic,
                                                 std::span<const std::uint8_t> key,
                                                 std::span<const std::uint8_t> message);
DecodeResult decode_produce_request(std::span<const std::uint8_t> payload, ProduceRequest& request);
DecodeResult validate_produce_request(std::span<const std::uint8_t> payload);

std::vector<std::uint8_t> encode_fetch_request(std::string_view topic, std::uint16_t partition,
                                               std::string_view from, std::string_view group,
                                               std::uint32_t max_wait_ms);
DecodeResult decode_fetch_request(std::span<const std::uint8_t> payload, FetchRequest& request);
DecodeResult validate_fetch_request(std::span<const std::uint8_t> payload);

std::vector<std::uint8_t> encode_offset_commit_request(const OffsetCommitRequest& request);
DecodeResult decode_offset_commit_request(std::span<const std::uint8_t> payload,
                                          OffsetCommitRequest& request);
std::vector<std::uint8_t> encode_offset_commit_response(const OffsetCommitResponse& response);
DecodeResult decode_offset_commit_response(std::span<const std::uint8_t> payload,
                                           OffsetCommitResponse& response);

std::vector<std::uint8_t> encode_join_group_request(const JoinGroupRequest& request);
DecodeResult decode_join_group_request(std::span<const std::uint8_t> payload,
                                       JoinGroupRequest& request);
std::vector<std::uint8_t> encode_join_group_response(const JoinGroupResponse& response);
DecodeResult decode_join_group_response(std::span<const std::uint8_t> payload,
                                        JoinGroupResponse& response);

std::vector<std::uint8_t> encode_sync_group_request(const SyncGroupRequest& request);
DecodeResult decode_sync_group_request(std::span<const std::uint8_t> payload,
                                       SyncGroupRequest& request);
std::vector<std::uint8_t> encode_sync_group_response(const SyncGroupResponse& response);
DecodeResult decode_sync_group_response(std::span<const std::uint8_t> payload,
                                        SyncGroupResponse& response);

std::vector<std::uint8_t> encode_heartbeat_request(const HeartbeatRequest& request);
DecodeResult decode_heartbeat_request(std::span<const std::uint8_t> payload,
                                      HeartbeatRequest& request);
std::vector<std::uint8_t> encode_heartbeat_response(const HeartbeatResponse& response);
DecodeResult decode_heartbeat_response(std::span<const std::uint8_t> payload,
                                       HeartbeatResponse& response);

std::vector<std::uint8_t> encode_leave_group_request(const LeaveGroupRequest& request);
DecodeResult decode_leave_group_request(std::span<const std::uint8_t> payload,
                                        LeaveGroupRequest& request);
std::vector<std::uint8_t> encode_leave_group_response(const LeaveGroupResponse& response);
DecodeResult decode_leave_group_response(std::span<const std::uint8_t> payload,
                                         LeaveGroupResponse& response);

std::vector<std::uint8_t>
encode_group_offset_commit_request(const GroupOffsetCommitRequest& request);
DecodeResult decode_group_offset_commit_request(std::span<const std::uint8_t> payload,
                                                GroupOffsetCommitRequest& request);
std::vector<std::uint8_t>
encode_group_offset_commit_response(const GroupOffsetCommitResponse& response);
DecodeResult decode_group_offset_commit_response(std::span<const std::uint8_t> payload,
                                                 GroupOffsetCommitResponse& response);

std::vector<std::uint8_t> encode_produce_response(const ProduceResponse& response);
DecodeResult decode_produce_response(std::span<const std::uint8_t> payload,
                                     ProduceResponse& response);

std::vector<std::uint8_t> encode_fetch_response(const FetchResponse& response);
DecodeResult decode_fetch_response(std::span<const std::uint8_t> payload, FetchResponse& response);

std::vector<std::uint8_t> encode_metadata_response(std::span<const MetadataTopic> topics);
DecodeResult decode_metadata_response(std::span<const std::uint8_t> payload,
                                      MetadataResponse& response);

DecodeResult validate_empty_payload(std::span<const std::uint8_t> payload);

} // namespace boltstream::protocol
