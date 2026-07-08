#include "boltstream/broker/server.h"
#include "boltstream/build_info.h"
#include "boltstream/client/async_client.h"

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct RunningServer {
  std::filesystem::path data_dir;
  std::unique_ptr<boltstream::broker::BrokerServer> server;
  std::thread thread;
  std::uint16_t port{0};

  ~RunningServer() {
    if (server) {
      server->stop();
    }
    if (thread.joinable()) {
      thread.join();
    }
    if (!data_dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(data_dir, ignored);
    }
  }
};

std::unique_ptr<RunningServer> start_server() {
  auto running = std::make_unique<RunningServer>();
  running->data_dir = std::filesystem::temp_directory_path() /
                      ("boltstream-phase2-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  boltstream::broker::ServerOptions options;
  options.listen = {"127.0.0.1", 0};
  options.admin_listen = {"127.0.0.1", 0};
  options.data_dir = running->data_dir;
  options.max_frame_bytes = boltstream::protocol::kDefaultMaxFrameBytes;

  running->server =
      std::make_unique<boltstream::broker::BrokerServer>(options, boltstream::current_build_info());
  running->server->start();
  running->port = running->server->broker_port();
  running->thread =
      std::thread([server = running->server.get()] { server->wait_for_shutdown_signal(); });
  return running;
}

boltstream::protocol::ErrorResponse decode_error(const boltstream::protocol::Frame& frame) {
  boltstream::protocol::ErrorResponse response;
  const auto decoded = boltstream::protocol::decode_error_response(frame.payload, response);
  EXPECT_TRUE(decoded.ok) << decoded.message;
  return response;
}

} // namespace

TEST(ClientBrokerTests, HealthRequestReturnsReady) {
  const auto running = start_server();
  boost::asio::io_context io;
  boltstream::client::AsyncClient client{io};
  boost::asio::steady_timer timer{io};
  boost::system::error_code seen_error;
  boltstream::protocol::Frame seen_frame;
  bool timed_out = false;

  timer.expires_after(std::chrono::seconds(2));
  timer.async_wait([&](const boost::system::error_code& ec) {
    if (!ec) {
      timed_out = true;
      client.close();
      io.stop();
    }
  });

  client.async_connect("127.0.0.1", running->port, [&](const boost::system::error_code& ec) {
    ASSERT_FALSE(ec) << ec.message();
    client.async_health(
        [&](const boost::system::error_code& request_ec, boltstream::protocol::Frame frame) {
          seen_error = request_ec;
          seen_frame = std::move(frame);
          timer.cancel();
          io.stop();
        });
  });
  io.run();

  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(seen_frame.header.frame_type, boltstream::protocol::FrameType::HealthResponse);

  boltstream::protocol::HealthResponse response;
  const auto decoded = boltstream::protocol::decode_health_response(seen_frame.payload, response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(response.status, "ready");
}

TEST(ClientBrokerTests, ProduceAndFetchReturnNotImplemented) {
  const auto running = start_server();
  boost::asio::io_context io;
  boltstream::client::AsyncClient client{io};
  boost::asio::steady_timer timer{io};
  std::vector<boltstream::protocol::Frame> frames;
  bool timed_out = false;

  timer.expires_after(std::chrono::seconds(2));
  timer.async_wait([&](const boost::system::error_code& ec) {
    if (!ec) {
      timed_out = true;
      client.close();
      io.stop();
    }
  });

  const std::vector<std::uint8_t> key{'A', 'A', 'P', 'L'};
  const std::vector<std::uint8_t> message{'1', '0', '0'};
  client.async_connect("127.0.0.1", running->port, [&](const boost::system::error_code& ec) {
    ASSERT_FALSE(ec) << ec.message();
    client.async_produce(
        "trades", key, message,
        [&](const boost::system::error_code& request_ec, boltstream::protocol::Frame frame) {
          ASSERT_FALSE(request_ec) << request_ec.message();
          frames.push_back(std::move(frame));
          if (frames.size() == 2) {
            timer.cancel();
            io.stop();
          }
        });
    client.async_fetch(
        "trades", "beginning",
        [&](const boost::system::error_code& request_ec, boltstream::protocol::Frame frame) {
          ASSERT_FALSE(request_ec) << request_ec.message();
          frames.push_back(std::move(frame));
          if (frames.size() == 2) {
            timer.cancel();
            io.stop();
          }
        });
  });
  io.run();

  ASSERT_FALSE(timed_out);
  ASSERT_EQ(frames.size(), 2U);
  for (const auto& frame : frames) {
    ASSERT_EQ(frame.header.frame_type, boltstream::protocol::FrameType::ErrorResponse);
    const auto response = decode_error(frame);
    EXPECT_EQ(response.code, boltstream::protocol::ErrorCode::NotImplemented);
  }
}

TEST(ClientBrokerTests, MultipleConcurrentRequestsPreserveCorrelationIds) {
  const auto running = start_server();
  boost::asio::io_context io;
  boltstream::client::AsyncClient client{io};
  boost::asio::steady_timer timer{io};
  std::vector<boltstream::protocol::Frame> frames;
  bool timed_out = false;

  timer.expires_after(std::chrono::seconds(2));
  timer.async_wait([&](const boost::system::error_code& ec) {
    if (!ec) {
      timed_out = true;
      client.close();
      io.stop();
    }
  });

  client.async_connect("127.0.0.1", running->port, [&](const boost::system::error_code& ec) {
    ASSERT_FALSE(ec) << ec.message();

    auto complete = [&](const boost::system::error_code& request_ec,
                        boltstream::protocol::Frame frame) {
      ASSERT_FALSE(request_ec) << request_ec.message();
      frames.push_back(std::move(frame));
      if (frames.size() == 3) {
        timer.cancel();
        io.stop();
      }
    };

    client.async_health(complete);
    client.async_metadata(complete);
    client.async_fetch("trades", "latest", complete);
  });
  io.run();

  ASSERT_FALSE(timed_out);
  ASSERT_EQ(frames.size(), 3U);
  EXPECT_EQ(frames[0].header.correlation_id, 1U);
  EXPECT_EQ(frames[1].header.correlation_id, 2U);
  EXPECT_EQ(frames[2].header.correlation_id, 3U);
}
