#include "boltstream/client/async_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <utility>

namespace boltstream::client {
namespace {

boost::system::error_code protocol_error() {
  return make_error_code(boost::asio::error::invalid_argument);
}

boost::system::error_code message_size_error() {
  return make_error_code(boost::asio::error::message_size);
}

} // namespace

AsyncClient::AsyncClient(boost::asio::io_context& io, std::uint32_t max_frame_bytes)
    : strand_(boost::asio::make_strand(io)), resolver_(strand_), socket_(strand_),
      max_frame_bytes_(max_frame_bytes) {}

AsyncClient::~AsyncClient() { close(); }

void AsyncClient::async_connect_impl(std::string host, std::uint16_t port, ConnectHandler handler) {
  boost::asio::post(strand_, [this, host = std::move(host), port,
                              handler = std::move(handler)]() mutable {
    resolver_.async_resolve(
        std::move(host), std::to_string(port),
        [this, handler = std::move(handler)](
            const boost::system::error_code& ec,
            const boost::asio::ip::tcp::resolver::results_type& endpoints) mutable {
          if (ec) {
            handler(ec);
            return;
          }
          boost::asio::async_connect(
              socket_, endpoints,
              [this, handler = std::move(handler)](const boost::system::error_code& connect_ec,
                                                   const boost::asio::ip::tcp::endpoint&) mutable {
                if (!connect_ec) {
                  start_read_header();
                }
                handler(connect_ec);
              });
        });
  });
}

void AsyncClient::async_request_impl(protocol::FrameType frame_type,
                                     std::vector<std::uint8_t> payload, ResponseHandler handler) {
  boost::asio::post(strand_, [this, frame_type, payload = std::move(payload),
                              handler = std::move(handler)]() mutable {
    if (!socket_.is_open()) {
      handler(make_error_code(boost::asio::error::not_connected), {});
      return;
    }
    if (max_frame_bytes_ < protocol::kFrameHeaderBytes ||
        payload.size() > max_frame_bytes_ - protocol::kFrameHeaderBytes) {
      handler(message_size_error(), {});
      return;
    }

    const auto correlation_id = next_correlation_id_++;
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(
        protocol::encode_frame(frame_type, correlation_id, payload));
    pending_.emplace(correlation_id, std::move(handler));
    write_queue_.push_back(bytes);
    if (write_queue_.size() == 1) {
      start_write();
    }
  });
}

void AsyncClient::start_write() {
  if (write_queue_.empty()) {
    return;
  }
  auto bytes = write_queue_.front();
  boost::asio::async_write(socket_, boost::asio::buffer(*bytes),
                           [this, bytes](const boost::system::error_code& ec, std::size_t) {
                             if (ec) {
                               fail_pending(ec);
                               close();
                               return;
                             }
                             write_queue_.pop_front();
                             start_write();
                           });
}

void AsyncClient::start_read_header() {
  boost::asio::async_read(socket_, boost::asio::buffer(header_buffer_),
                          [this](const boost::system::error_code& ec, std::size_t) {
                            if (ec) {
                              fail_pending(ec);
                              return;
                            }
                            const auto decoded =
                                protocol::decode_header(header_buffer_, max_frame_bytes_);
                            if (!decoded.ok) {
                              fail_pending(protocol_error());
                              close();
                              return;
                            }
                            start_read_payload(decoded.header);
                          });
}

void AsyncClient::start_read_payload(protocol::FrameHeader header) {
  payload_buffer_.assign(header.payload_bytes, 0);
  boost::asio::async_read(socket_, boost::asio::buffer(payload_buffer_),
                          [this, header](const boost::system::error_code& ec, std::size_t) mutable {
                            if (ec) {
                              fail_pending(ec);
                              return;
                            }
                            protocol::Frame frame;
                            frame.header = header;
                            frame.payload = std::move(payload_buffer_);
                            dispatch_response(std::move(frame));
                            start_read_header();
                          });
}

void AsyncClient::dispatch_response(protocol::Frame frame) {
  const auto it = pending_.find(frame.header.correlation_id);
  if (it == pending_.end()) {
    return;
  }
  auto handler = std::move(it->second);
  pending_.erase(it);
  handler({}, std::move(frame));
}

void AsyncClient::fail_pending(boost::system::error_code ec) {
  auto pending = std::move(pending_);
  pending_.clear();
  for (auto& [correlation_id, handler] : pending) {
    (void)correlation_id;
    handler(ec, {});
  }
}

void AsyncClient::close() {
  boost::system::error_code ignored;
  resolver_.cancel();
  if (socket_.is_open()) {
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }
}

} // namespace boltstream::client
