#pragma once

#include "boltstream/protocol/protocol.h"

#include <boost/asio.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace boltstream::client {

class AsyncClient {
public:
  explicit AsyncClient(boost::asio::io_context& io,
                       std::uint32_t max_frame_bytes = protocol::kDefaultMaxFrameBytes,
                       std::uint16_t protocol_version = protocol::kProtocolVersion);
  AsyncClient(const AsyncClient&) = delete;
  AsyncClient& operator=(const AsyncClient&) = delete;
  ~AsyncClient();

  template <typename CompletionToken>
  auto async_connect(std::string host, std::uint16_t port, CompletionToken&& token) {
    return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
        [this, host = std::move(host), port](auto&& handler) mutable {
          using Handler = std::decay_t<decltype(handler)>;
          auto shared_handler = std::make_shared<Handler>(std::forward<decltype(handler)>(handler));
          async_connect_impl(std::move(host), port,
                             [shared_handler](boost::system::error_code ec) mutable {
                               auto handler = std::move(*shared_handler);
                               handler(ec);
                             });
        },
        token);
  }

  template <typename CompletionToken>
  auto async_request(protocol::FrameType frame_type, std::vector<std::uint8_t> payload,
                     CompletionToken&& token) {
    return boost::asio::async_initiate<CompletionToken,
                                       void(boost::system::error_code, protocol::Frame)>(
        [this, frame_type, payload = std::move(payload)](auto&& handler) mutable {
          using Handler = std::decay_t<decltype(handler)>;
          auto shared_handler = std::make_shared<Handler>(std::forward<decltype(handler)>(handler));
          async_request_impl(
              frame_type, std::move(payload),
              [shared_handler](boost::system::error_code ec, protocol::Frame frame) mutable {
                auto handler = std::move(*shared_handler);
                handler(ec, std::move(frame));
              });
        },
        token);
  }

  template <typename CompletionToken> auto async_health(CompletionToken&& token) {
    return async_request(protocol::FrameType::HealthRequest, {},
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_metadata(CompletionToken&& token,
                      std::uint32_t codecs = compression::kSupportedCodecMask) {
    return async_request(protocol::FrameType::MetadataRequest,
                         protocol_version_ >= 5 ? protocol::encode_metadata_request(codecs)
                                                : std::vector<std::uint8_t>{},
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_auth(std::string_view broker_token, CompletionToken&& token) {
    return async_request(protocol::FrameType::AuthRequest,
                         protocol::encode_auth_request(broker_token),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_produce(std::string_view topic, std::span<const std::uint8_t> key,
                     std::span<const std::uint8_t> message, CompletionToken&& token) {
    return async_request(protocol::FrameType::ProduceRequest,
                         protocol::encode_produce_request(topic, key, message),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_produce_batch(const protocol::ProduceBatchRequest& request, CompletionToken&& token) {
    return async_request(protocol::FrameType::ProduceBatchRequest,
                         protocol::encode_produce_batch_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_create_topic(std::string_view topic, std::uint16_t partition_count,
                          CompletionToken&& token) {
    return async_request(protocol::FrameType::CreateTopicRequest,
                         protocol::encode_create_topic_request(topic, partition_count),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_fetch(std::string_view topic, std::uint16_t partition, std::string_view from,
                   std::string_view group, std::uint32_t max_wait_ms, CompletionToken&& token) {
    return async_request(protocol::FrameType::FetchRequest,
                         protocol::encode_fetch_request(topic, partition, from, group, max_wait_ms),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_offset_commit(const protocol::OffsetCommitRequest& request, CompletionToken&& token) {
    return async_request(protocol::FrameType::OffsetCommitRequest,
                         protocol::encode_offset_commit_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_join_group(const protocol::JoinGroupRequest& request, CompletionToken&& token) {
    return async_request(protocol::FrameType::JoinGroupRequest,
                         protocol::encode_join_group_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_sync_group(const protocol::SyncGroupRequest& request, CompletionToken&& token) {
    return async_request(protocol::FrameType::SyncGroupRequest,
                         protocol::encode_sync_group_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_heartbeat(const protocol::HeartbeatRequest& request, CompletionToken&& token) {
    return async_request(protocol::FrameType::HeartbeatRequest,
                         protocol::encode_heartbeat_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_leave_group(const protocol::LeaveGroupRequest& request, CompletionToken&& token) {
    return async_request(protocol::FrameType::LeaveGroupRequest,
                         protocol::encode_leave_group_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_group_offset_commit(const protocol::GroupOffsetCommitRequest& request,
                                 CompletionToken&& token) {
    return async_request(protocol::FrameType::GroupOffsetCommitRequest,
                         protocol::encode_group_offset_commit_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken> auto async_list_topics(CompletionToken&& token) {
    return async_request(protocol::FrameType::ListTopicsRequest, {},
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_describe_topic(const protocol::DescribeTopicRequest& request,
                            CompletionToken&& token) {
    return async_request(protocol::FrameType::DescribeTopicRequest,
                         protocol::encode_describe_topic_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_delete_topic(const protocol::DeleteTopicRequest& request, CompletionToken&& token) {
    return async_request(protocol::FrameType::DeleteTopicRequest,
                         protocol::encode_delete_topic_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_run_retention(const protocol::RunRetentionRequest& request, CompletionToken&& token) {
    return async_request(protocol::FrameType::RunRetentionRequest,
                         protocol::encode_run_retention_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_describe_group(const protocol::DescribeGroupRequest& request,
                            CompletionToken&& token) {
    return async_request(protocol::FrameType::DescribeGroupRequest,
                         protocol::encode_describe_group_request(request),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_reset_group_offset(const protocol::ResetGroupOffsetRequest& request,
                                CompletionToken&& token) {
    return async_request(protocol::FrameType::ResetGroupOffsetRequest,
                         protocol::encode_reset_group_offset_request(request),
                         std::forward<CompletionToken>(token));
  }

  void close();

private:
  using ResponseHandler = std::function<void(boost::system::error_code, protocol::Frame)>;
  using ConnectHandler = std::function<void(boost::system::error_code)>;

  void async_connect_impl(std::string host, std::uint16_t port, ConnectHandler handler);
  void async_request_impl(protocol::FrameType frame_type, std::vector<std::uint8_t> payload,
                          ResponseHandler handler);
  void start_write();
  void start_read_header();
  void start_read_payload(protocol::FrameHeader header);
  void dispatch_response(protocol::Frame frame);
  void fail_pending(boost::system::error_code ec);

  boost::asio::strand<boost::asio::any_io_executor> strand_;
  boost::asio::ip::tcp::resolver resolver_;
  boost::asio::ip::tcp::socket socket_;
  std::uint32_t max_frame_bytes_;
  std::uint16_t protocol_version_;
  std::uint64_t next_correlation_id_{1};
  std::array<std::uint8_t, protocol::kFrameHeaderBytes> header_buffer_{};
  std::vector<std::uint8_t> payload_buffer_;
  std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
  std::unordered_map<std::uint64_t, ResponseHandler> pending_;
};

} // namespace boltstream::client
