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
                       std::uint32_t max_frame_bytes = protocol::kDefaultMaxFrameBytes);
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

  template <typename CompletionToken> auto async_metadata(CompletionToken&& token) {
    return async_request(protocol::FrameType::MetadataRequest, {},
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
  auto async_fetch(std::string_view topic, std::string_view from, CompletionToken&& token) {
    return async_request(protocol::FrameType::FetchRequest,
                         protocol::encode_fetch_request(topic, from),
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

  boost::asio::io_context& io_;
  boost::asio::ip::tcp::resolver resolver_;
  boost::asio::ip::tcp::socket socket_;
  std::uint32_t max_frame_bytes_;
  std::uint64_t next_correlation_id_{1};
  std::array<std::uint8_t, protocol::kFrameHeaderBytes> header_buffer_{};
  std::vector<std::uint8_t> payload_buffer_;
  std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
  std::unordered_map<std::uint64_t, ResponseHandler> pending_;
};

} // namespace boltstream::client
