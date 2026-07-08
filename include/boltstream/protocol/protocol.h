#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boltstream::protocol {

inline constexpr std::uint32_t kMagic = 0x42535452U;
inline constexpr std::uint16_t kProtocolVersion = 1;
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

struct ProduceRequest {
  std::string topic;
  std::vector<std::uint8_t> key;
  std::vector<std::uint8_t> message;
};

struct FetchRequest {
  std::string topic;
  std::string from;
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

std::vector<std::uint8_t> encode_produce_request(std::string_view topic,
                                                 std::span<const std::uint8_t> key,
                                                 std::span<const std::uint8_t> message);
DecodeResult decode_produce_request(std::span<const std::uint8_t> payload, ProduceRequest& request);
DecodeResult validate_produce_request(std::span<const std::uint8_t> payload);

std::vector<std::uint8_t> encode_fetch_request(std::string_view topic, std::string_view from);
DecodeResult decode_fetch_request(std::span<const std::uint8_t> payload, FetchRequest& request);
DecodeResult validate_fetch_request(std::span<const std::uint8_t> payload);

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
