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
      boltstream::protocol::ErrorCode::NotImplemented, "produce storage is implemented in Phase 4");
  boltstream::protocol::ErrorResponse response;

  const auto decoded = boltstream::protocol::decode_error_response(payload, response);

  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(response.code, boltstream::protocol::ErrorCode::NotImplemented);
  EXPECT_EQ(response.message, "produce storage is implemented in Phase 4");
}
