#include "boltstream/protocol/protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
  bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xFFU);
}

void refresh_header_crc(std::vector<std::uint8_t>& bytes) {
  const auto crc = boltstream::protocol::crc32(
      std::span<const std::uint8_t>{bytes.data(), boltstream::protocol::kFrameHeaderBytes - 4});
  put_u32(bytes, boltstream::protocol::kFrameHeaderBytes - 4, crc);
}

} // namespace

TEST(ProtocolTests, ValidFrameRoundTripsWithCorrelationId) {
  const auto payload = boltstream::protocol::encode_health_response("ready", "ready");
  const auto bytes = boltstream::protocol::encode_frame(
      boltstream::protocol::FrameType::HealthResponse, 42, payload);

  const auto decoded =
      boltstream::protocol::decode_frame(bytes, boltstream::protocol::kDefaultMaxFrameBytes);

  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded.frame.header.magic, boltstream::protocol::kMagic);
  EXPECT_EQ(decoded.frame.header.version, boltstream::protocol::kProtocolVersion);
  EXPECT_EQ(decoded.frame.header.frame_type, boltstream::protocol::FrameType::HealthResponse);
  EXPECT_EQ(decoded.frame.header.correlation_id, 42U);
  EXPECT_EQ(decoded.frame.payload, payload);
}

TEST(ProtocolTests, RejectsTruncatedHeader) {
  const std::vector<std::uint8_t> bytes{0x42, 0x53, 0x54};

  const auto decoded =
      boltstream::protocol::decode_header(bytes, boltstream::protocol::kDefaultMaxFrameBytes);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::InvalidLength);
}

TEST(ProtocolTests, RejectsInvalidMagic) {
  auto bytes =
      boltstream::protocol::encode_frame(boltstream::protocol::FrameType::HealthRequest, 7, {});
  bytes[0] = 0x00;
  refresh_header_crc(bytes);

  const auto decoded =
      boltstream::protocol::decode_header(bytes, boltstream::protocol::kDefaultMaxFrameBytes);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::InvalidMagic);
}

TEST(ProtocolTests, RejectsInvalidLength) {
  auto bytes =
      boltstream::protocol::encode_frame(boltstream::protocol::FrameType::HealthRequest, 7, {});
  put_u32(bytes, 8, boltstream::protocol::kFrameHeaderBytes - 1);
  refresh_header_crc(bytes);

  const auto decoded =
      boltstream::protocol::decode_frame(bytes, boltstream::protocol::kDefaultMaxFrameBytes);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::InvalidLength);
}

TEST(ProtocolTests, RejectsUnsupportedVersion) {
  auto bytes =
      boltstream::protocol::encode_frame(boltstream::protocol::FrameType::HealthRequest, 7, {});
  bytes[5] = 2;
  refresh_header_crc(bytes);

  const auto decoded =
      boltstream::protocol::decode_header(bytes, boltstream::protocol::kDefaultMaxFrameBytes);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::UnsupportedVersion);
}

TEST(ProtocolTests, RejectsHeaderCrcMismatch) {
  auto bytes =
      boltstream::protocol::encode_frame(boltstream::protocol::FrameType::HealthRequest, 7, {});
  bytes.back() ^= 0xFFU;

  const auto decoded =
      boltstream::protocol::decode_header(bytes, boltstream::protocol::kDefaultMaxFrameBytes);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::CrcMismatch);
}

TEST(ProtocolTests, RejectsReservedFlags) {
  auto bytes =
      boltstream::protocol::encode_frame(boltstream::protocol::FrameType::HealthRequest, 7, {});
  bytes[27] = 1;
  refresh_header_crc(bytes);

  const auto decoded =
      boltstream::protocol::decode_header(bytes, boltstream::protocol::kDefaultMaxFrameBytes);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::ReservedFlags);
}

TEST(ProtocolTests, RejectsMalformedProducePayload) {
  const std::vector<std::uint8_t> payload{0, 0, 0, 5, 't', 'r'};

  const auto decoded = boltstream::protocol::validate_produce_request(payload);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::MalformedPayload);
}

TEST(ProtocolTests, RejectsFrameAboveConfiguredMaximum) {
  const auto bytes =
      boltstream::protocol::encode_frame(boltstream::protocol::FrameType::HealthRequest, 7, {});

  const auto decoded =
      boltstream::protocol::decode_header(bytes, boltstream::protocol::kFrameHeaderBytes - 1);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::InvalidLength);
}

TEST(ProtocolTests, DecodesErrorResponsePayload) {
  const auto payload = boltstream::protocol::encode_error_response(
      boltstream::protocol::ErrorCode::NotImplemented,
      "offset commits are implemented with consumer groups");
  boltstream::protocol::ErrorResponse response;

  const auto decoded = boltstream::protocol::decode_error_response(payload, response);

  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(response.code, boltstream::protocol::ErrorCode::NotImplemented);
  EXPECT_EQ(response.message, "offset commits are implemented with consumer groups");
}

TEST(ProtocolTests, DecodesUnauthorizedErrorResponsePayload) {
  const auto payload = boltstream::protocol::encode_error_response(
      boltstream::protocol::ErrorCode::Unauthorized, "authentication required");
  boltstream::protocol::ErrorResponse response;

  const auto decoded = boltstream::protocol::decode_error_response(payload, response);

  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(response.code, boltstream::protocol::ErrorCode::Unauthorized);
  EXPECT_EQ(boltstream::protocol::error_code_name(response.code), "unauthorized");
}

TEST(ProtocolTests, PhaseFourPayloadsRoundTrip) {
  const auto auth_request_payload = boltstream::protocol::encode_auth_request("secret-token");
  boltstream::protocol::AuthRequest auth_request;
  auto decoded = boltstream::protocol::decode_auth_request(auth_request_payload, auth_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(auth_request.token, "secret-token");

  const auto auth_response_payload = boltstream::protocol::encode_auth_response("authenticated");
  boltstream::protocol::AuthResponse auth_response;
  decoded = boltstream::protocol::decode_auth_response(auth_response_payload, auth_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(auth_response.status, "authenticated");

  const std::vector<std::uint8_t> request_key{'A'};
  const std::vector<std::uint8_t> request_message{'1'};
  const auto produce_request_payload =
      boltstream::protocol::encode_produce_request("trades", request_key, request_message);
  boltstream::protocol::ProduceRequest produce_request;
  decoded = boltstream::protocol::decode_produce_request(produce_request_payload, produce_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(produce_request.topic, "trades");
  EXPECT_EQ(produce_request.key, (std::vector<std::uint8_t>{'A'}));
  EXPECT_EQ(produce_request.message, (std::vector<std::uint8_t>{'1'}));

  const boltstream::protocol::ProduceResponse produce_response{"trades", 0, 4, 5, 64};
  const auto produce_response_payload =
      boltstream::protocol::encode_produce_response(produce_response);
  boltstream::protocol::ProduceResponse decoded_produce_response;
  decoded = boltstream::protocol::decode_produce_response(produce_response_payload,
                                                          decoded_produce_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_produce_response.topic, "trades");
  EXPECT_EQ(decoded_produce_response.offset, 4U);
  EXPECT_EQ(decoded_produce_response.next_offset, 5U);

  boltstream::protocol::FetchResponse fetch_response;
  fetch_response.topic = "trades";
  fetch_response.partition = 0;
  fetch_response.from_offset = 4;
  fetch_response.next_offset = 5;
  fetch_response.records.push_back(
      {4, 123, std::vector<std::uint8_t>{'A'}, std::vector<std::uint8_t>{'1'}, 64});
  const auto fetch_response_payload = boltstream::protocol::encode_fetch_response(fetch_response);
  boltstream::protocol::FetchResponse decoded_fetch_response;
  decoded =
      boltstream::protocol::decode_fetch_response(fetch_response_payload, decoded_fetch_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(decoded_fetch_response.records.size(), 1U);
  EXPECT_EQ(decoded_fetch_response.records[0].offset, 4U);
  EXPECT_EQ(decoded_fetch_response.records[0].key, (std::vector<std::uint8_t>{'A'}));
  EXPECT_EQ(decoded_fetch_response.records[0].message, (std::vector<std::uint8_t>{'1'}));

  const std::vector<boltstream::protocol::MetadataTopic> topics{{"trades", 0, 5}};
  const auto metadata_payload = boltstream::protocol::encode_metadata_response(topics);
  boltstream::protocol::MetadataResponse metadata_response;
  decoded = boltstream::protocol::decode_metadata_response(metadata_payload, metadata_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(metadata_response.topics.size(), 1U);
  EXPECT_EQ(metadata_response.topics[0].topic, "trades");
  EXPECT_EQ(metadata_response.topics[0].next_offset, 5U);
}
