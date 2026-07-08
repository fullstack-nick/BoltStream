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
  bytes[5] = 3;
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

TEST(ProtocolTests, OverloadedErrorIsRetryable) {
  const auto payload = boltstream::protocol::encode_error_response(
      boltstream::protocol::ErrorCode::Overloaded, "append queue is full");
  boltstream::protocol::ErrorResponse response;

  const auto decoded = boltstream::protocol::decode_error_response(payload, response);

  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(response.code, boltstream::protocol::ErrorCode::Overloaded);
  EXPECT_EQ(boltstream::protocol::error_code_name(response.code), "overloaded");
  EXPECT_TRUE(boltstream::protocol::is_retryable_error(response.code));
  EXPECT_FALSE(
      boltstream::protocol::is_retryable_error(boltstream::protocol::ErrorCode::MalformedPayload));
}

TEST(ProtocolTests, PhaseSevenErrorsAndFrameNamesAreStable) {
  EXPECT_EQ(boltstream::protocol::kProtocolVersion, 4U);
  EXPECT_EQ(
      boltstream::protocol::frame_type_name(boltstream::protocol::FrameType::JoinGroupRequest),
      "join_group_request");
  EXPECT_EQ(boltstream::protocol::frame_type_name(
                boltstream::protocol::FrameType::GroupOffsetCommitResponse),
            "group_offset_commit_response");
  EXPECT_EQ(
      boltstream::protocol::error_code_name(boltstream::protocol::ErrorCode::RebalanceRequired),
      "rebalance_required");
  EXPECT_EQ(boltstream::protocol::error_code_name(boltstream::protocol::ErrorCode::StaleMember),
            "stale_member");
  EXPECT_TRUE(
      boltstream::protocol::is_retryable_error(boltstream::protocol::ErrorCode::RebalanceRequired));
  EXPECT_FALSE(
      boltstream::protocol::is_retryable_error(boltstream::protocol::ErrorCode::StaleMember));
}

TEST(ProtocolTests, PhaseEightErrorsAndFrameNamesAreStable) {
  EXPECT_EQ(
      boltstream::protocol::frame_type_name(boltstream::protocol::FrameType::ListTopicsRequest),
      "list_topics_request");
  EXPECT_EQ(boltstream::protocol::frame_type_name(
                boltstream::protocol::FrameType::ResetGroupOffsetResponse),
            "reset_group_offset_response");
  EXPECT_EQ(
      boltstream::protocol::error_code_name(boltstream::protocol::ErrorCode::OffsetOutOfRange),
      "offset_out_of_range");
  EXPECT_EQ(boltstream::protocol::error_code_name(boltstream::protocol::ErrorCode::GroupActive),
            "group_active");
  EXPECT_FALSE(
      boltstream::protocol::is_retryable_error(boltstream::protocol::ErrorCode::OffsetOutOfRange));
  EXPECT_FALSE(
      boltstream::protocol::is_retryable_error(boltstream::protocol::ErrorCode::GroupActive));
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

  const auto create_payload = boltstream::protocol::encode_create_topic_request("trades", 3);
  boltstream::protocol::CreateTopicRequest create_request;
  decoded = boltstream::protocol::decode_create_topic_request(create_payload, create_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(create_request.topic, "trades");
  EXPECT_EQ(create_request.partition_count, 3U);

  const boltstream::protocol::CreateTopicResponse create_response{"trades", 3, "created"};
  const auto create_response_payload =
      boltstream::protocol::encode_create_topic_response(create_response);
  boltstream::protocol::CreateTopicResponse decoded_create_response;
  decoded = boltstream::protocol::decode_create_topic_response(create_response_payload,
                                                               decoded_create_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_create_response.status, "created");

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

  const auto fetch_request_payload =
      boltstream::protocol::encode_fetch_request("trades", 2, "committed", "dashboard", 250);
  boltstream::protocol::FetchRequest fetch_request;
  decoded = boltstream::protocol::decode_fetch_request(fetch_request_payload, fetch_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(fetch_request.topic, "trades");
  EXPECT_EQ(fetch_request.partition, 2U);
  EXPECT_EQ(fetch_request.from, "committed");
  EXPECT_EQ(fetch_request.group, "dashboard");
  EXPECT_EQ(fetch_request.max_wait_ms, 250U);

  const boltstream::protocol::OffsetCommitRequest commit_request{"dashboard", "trades", 2, 11};
  const auto commit_request_payload =
      boltstream::protocol::encode_offset_commit_request(commit_request);
  boltstream::protocol::OffsetCommitRequest decoded_commit_request;
  decoded = boltstream::protocol::decode_offset_commit_request(commit_request_payload,
                                                               decoded_commit_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_commit_request.next_offset, 11U);

  const boltstream::protocol::OffsetCommitResponse commit_response{"dashboard", "trades", 2, 11};
  const auto commit_response_payload =
      boltstream::protocol::encode_offset_commit_response(commit_response);
  boltstream::protocol::OffsetCommitResponse decoded_commit_response;
  decoded = boltstream::protocol::decode_offset_commit_response(commit_response_payload,
                                                                decoded_commit_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_commit_response.group, "dashboard");

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

TEST(ProtocolTests, PhaseSevenGroupPayloadsRoundTrip) {
  boltstream::protocol::DecodeResult decoded;

  const boltstream::protocol::JoinGroupRequest join_request{"dashboard", "trades", "", 3000};
  const auto join_request_payload = boltstream::protocol::encode_join_group_request(join_request);
  boltstream::protocol::JoinGroupRequest decoded_join_request;
  decoded =
      boltstream::protocol::decode_join_group_request(join_request_payload, decoded_join_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_join_request.group, "dashboard");
  EXPECT_EQ(decoded_join_request.topic, "trades");
  EXPECT_EQ(decoded_join_request.member_id, "");
  EXPECT_EQ(decoded_join_request.session_timeout_ms, 3000U);

  const boltstream::protocol::JoinGroupResponse join_response{"dashboard", "trades",
                                                              "member-000000000001", 4};
  const auto join_response_payload =
      boltstream::protocol::encode_join_group_response(join_response);
  boltstream::protocol::JoinGroupResponse decoded_join_response;
  decoded = boltstream::protocol::decode_join_group_response(join_response_payload,
                                                             decoded_join_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_join_response.member_id, "member-000000000001");
  EXPECT_EQ(decoded_join_response.generation_id, 4U);

  const boltstream::protocol::SyncGroupRequest sync_request{"dashboard", "trades",
                                                            "member-000000000001", 4};
  const auto sync_request_payload = boltstream::protocol::encode_sync_group_request(sync_request);
  boltstream::protocol::SyncGroupRequest decoded_sync_request;
  decoded =
      boltstream::protocol::decode_sync_group_request(sync_request_payload, decoded_sync_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_sync_request.generation_id, 4U);

  const boltstream::protocol::SyncGroupResponse sync_response{
      "dashboard", "trades", "member-000000000001", 4, {0, 1}};
  const auto sync_response_payload =
      boltstream::protocol::encode_sync_group_response(sync_response);
  boltstream::protocol::SyncGroupResponse decoded_sync_response;
  decoded = boltstream::protocol::decode_sync_group_response(sync_response_payload,
                                                             decoded_sync_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_sync_response.assignment, (std::vector<std::uint16_t>{0, 1}));

  const boltstream::protocol::HeartbeatRequest heartbeat_request{"dashboard", "trades",
                                                                 "member-000000000001", 4};
  const auto heartbeat_request_payload =
      boltstream::protocol::encode_heartbeat_request(heartbeat_request);
  boltstream::protocol::HeartbeatRequest decoded_heartbeat_request;
  decoded = boltstream::protocol::decode_heartbeat_request(heartbeat_request_payload,
                                                           decoded_heartbeat_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;

  const boltstream::protocol::HeartbeatResponse heartbeat_response{"dashboard", "trades",
                                                                   "member-000000000001", 4, "ok"};
  const auto heartbeat_response_payload =
      boltstream::protocol::encode_heartbeat_response(heartbeat_response);
  boltstream::protocol::HeartbeatResponse decoded_heartbeat_response;
  decoded = boltstream::protocol::decode_heartbeat_response(heartbeat_response_payload,
                                                            decoded_heartbeat_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_heartbeat_response.status, "ok");

  const boltstream::protocol::LeaveGroupRequest leave_request{"dashboard", "trades",
                                                              "member-000000000001", 4};
  const auto leave_request_payload =
      boltstream::protocol::encode_leave_group_request(leave_request);
  boltstream::protocol::LeaveGroupRequest decoded_leave_request;
  decoded = boltstream::protocol::decode_leave_group_request(leave_request_payload,
                                                             decoded_leave_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;

  const boltstream::protocol::LeaveGroupResponse leave_response{"dashboard", "trades",
                                                                "member-000000000001", 5, "left"};
  const auto leave_response_payload =
      boltstream::protocol::encode_leave_group_response(leave_response);
  boltstream::protocol::LeaveGroupResponse decoded_leave_response;
  decoded = boltstream::protocol::decode_leave_group_response(leave_response_payload,
                                                              decoded_leave_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_leave_response.status, "left");

  const boltstream::protocol::GroupOffsetCommitRequest commit_request{
      "dashboard", "trades", "member-000000000001", 4, 1, 22};
  const auto commit_request_payload =
      boltstream::protocol::encode_group_offset_commit_request(commit_request);
  boltstream::protocol::GroupOffsetCommitRequest decoded_commit_request;
  decoded = boltstream::protocol::decode_group_offset_commit_request(commit_request_payload,
                                                                     decoded_commit_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_commit_request.partition, 1U);
  EXPECT_EQ(decoded_commit_request.next_offset, 22U);

  const boltstream::protocol::GroupOffsetCommitResponse commit_response{
      "dashboard", "trades", "member-000000000001", 4, 1, 22};
  const auto commit_response_payload =
      boltstream::protocol::encode_group_offset_commit_response(commit_response);
  boltstream::protocol::GroupOffsetCommitResponse decoded_commit_response;
  decoded = boltstream::protocol::decode_group_offset_commit_response(commit_response_payload,
                                                                      decoded_commit_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_commit_response.member_id, "member-000000000001");
}

TEST(ProtocolTests, PhaseEightLifecyclePayloadsRoundTrip) {
  boltstream::protocol::DecodeResult decoded;

  boltstream::protocol::ListTopicsResponse list_response;
  list_response.topics.push_back({"trades", 2, 128, {{0, 4, 10, 2, 64}, {1, 0, 3, 1, 64}}});
  const auto list_payload = boltstream::protocol::encode_list_topics_response(list_response);
  boltstream::protocol::ListTopicsResponse decoded_list;
  decoded = boltstream::protocol::decode_list_topics_response(list_payload, decoded_list);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(decoded_list.topics.size(), 1U);
  EXPECT_EQ(decoded_list.topics[0].topic, "trades");
  EXPECT_EQ(decoded_list.topics[0].partitions[0].earliest_offset, 4U);

  const boltstream::protocol::DescribeTopicRequest describe_topic_request{"trades"};
  const auto describe_topic_payload =
      boltstream::protocol::encode_describe_topic_request(describe_topic_request);
  boltstream::protocol::DescribeTopicRequest decoded_describe_topic_request;
  decoded = boltstream::protocol::decode_describe_topic_request(describe_topic_payload,
                                                                decoded_describe_topic_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_describe_topic_request.topic, "trades");

  const boltstream::protocol::DeleteTopicResponse delete_response{"trades", "deleted", 2,
                                                                  3,        512,       4};
  const auto delete_payload = boltstream::protocol::encode_delete_topic_response(delete_response);
  boltstream::protocol::DeleteTopicResponse decoded_delete;
  decoded = boltstream::protocol::decode_delete_topic_response(delete_payload, decoded_delete);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_delete.status, "deleted");
  EXPECT_EQ(decoded_delete.offsets_removed, 4U);

  boltstream::protocol::RunRetentionResponse retention_response;
  retention_response.topic = "trades";
  retention_response.topics_scanned = 1;
  retention_response.partitions_scanned = 1;
  retention_response.segments_deleted = 2;
  retention_response.bytes_deleted = 256;
  retention_response.partitions.push_back({"trades", 0, 2, 256, 8, 12});
  const auto retention_payload =
      boltstream::protocol::encode_run_retention_response(retention_response);
  boltstream::protocol::RunRetentionResponse decoded_retention;
  decoded =
      boltstream::protocol::decode_run_retention_response(retention_payload, decoded_retention);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(decoded_retention.partitions.size(), 1U);
  EXPECT_EQ(decoded_retention.partitions[0].earliest_offset, 8U);

  boltstream::protocol::DescribeGroupResponse group_response;
  group_response.group = "dashboard";
  group_response.topic = "trades";
  group_response.active_member_count = 0;
  group_response.offsets.push_back({0, true, 8, 4, 12, 4, false});
  const auto group_payload = boltstream::protocol::encode_describe_group_response(group_response);
  boltstream::protocol::DescribeGroupResponse decoded_group;
  decoded = boltstream::protocol::decode_describe_group_response(group_payload, decoded_group);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(decoded_group.offsets.size(), 1U);
  EXPECT_TRUE(decoded_group.offsets[0].has_committed_offset);
  EXPECT_EQ(decoded_group.offsets[0].lag, 4U);

  const boltstream::protocol::ResetGroupOffsetRequest reset_request{"dashboard", "trades", 0,
                                                                    "latest"};
  const auto reset_payload = boltstream::protocol::encode_reset_group_offset_request(reset_request);
  boltstream::protocol::ResetGroupOffsetRequest decoded_reset_request;
  decoded =
      boltstream::protocol::decode_reset_group_offset_request(reset_payload, decoded_reset_request);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_reset_request.to, "latest");

  const boltstream::protocol::ResetGroupOffsetResponse reset_response{"dashboard", "trades", 0, 12,
                                                                      "reset"};
  const auto reset_response_payload =
      boltstream::protocol::encode_reset_group_offset_response(reset_response);
  boltstream::protocol::ResetGroupOffsetResponse decoded_reset_response;
  decoded = boltstream::protocol::decode_reset_group_offset_response(reset_response_payload,
                                                                     decoded_reset_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(decoded_reset_response.next_offset, 12U);
}

TEST(ProtocolTests, RejectsMalformedPhaseSevenGroupPayloads) {
  boltstream::protocol::JoinGroupRequest join_request;
  auto decoded = boltstream::protocol::decode_join_group_request(
      boltstream::protocol::encode_join_group_request({"", "trades", "", 3000}), join_request);
  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::MalformedPayload);

  decoded = boltstream::protocol::decode_join_group_request(
      boltstream::protocol::encode_join_group_request({"dashboard", "trades", "", 0}),
      join_request);
  EXPECT_FALSE(decoded.ok);

  boltstream::protocol::SyncGroupRequest sync_request;
  decoded = boltstream::protocol::decode_sync_group_request(
      boltstream::protocol::encode_sync_group_request({"dashboard", "trades", "", 1}),
      sync_request);
  EXPECT_FALSE(decoded.ok);

  boltstream::protocol::GroupOffsetCommitRequest commit_request;
  decoded = boltstream::protocol::decode_group_offset_commit_request(
      boltstream::protocol::encode_group_offset_commit_request(
          {"dashboard", "trades", "member-000000000001", 0, 0, 1}),
      commit_request);
  EXPECT_FALSE(decoded.ok);
}

TEST(ProtocolTests, RejectsMalformedPhaseEightLifecyclePayloads) {
  boltstream::protocol::DescribeTopicRequest topic_request;
  auto decoded = boltstream::protocol::decode_describe_topic_request(
      boltstream::protocol::encode_describe_topic_request({""}), topic_request);
  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.error, boltstream::protocol::ErrorCode::MalformedPayload);

  boltstream::protocol::DescribeGroupRequest group_request;
  decoded = boltstream::protocol::decode_describe_group_request(
      boltstream::protocol::encode_describe_group_request({"", "trades"}), group_request);
  EXPECT_FALSE(decoded.ok);

  boltstream::protocol::ResetGroupOffsetRequest reset_request;
  decoded = boltstream::protocol::decode_reset_group_offset_request(
      boltstream::protocol::encode_reset_group_offset_request({"dashboard", "trades", 0, ""}),
      reset_request);
  EXPECT_FALSE(decoded.ok);
}
