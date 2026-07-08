#include "boltstream/broker/server.h"
#include "boltstream/build_info.h"
#include "boltstream/client/async_client.h"

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct RunningServer {
  std::filesystem::path data_dir;
  bool remove_data_dir{true};
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
    if (remove_data_dir && !data_dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(data_dir, ignored);
    }
  }
};

std::optional<std::string> get_env_var(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t value_size = 0;
  if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string out{value};
  std::free(value);
  return out;
#else
  const auto* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string{value};
#endif
}

void set_env_var(const char* name, const std::optional<std::string>& value) {
#if defined(_WIN32)
  _putenv_s(name, value ? value->c_str() : "");
#else
  if (value) {
    setenv(name, value->c_str(), 1);
  } else {
    unsetenv(name);
  }
#endif
}

struct ScopedEnvVar {
  const char* name;
  std::optional<std::string> previous;

  ScopedEnvVar(const char* env_name, std::optional<std::string> value)
      : name(env_name), previous(get_env_var(env_name)) {
    set_env_var(name, value);
  }

  ~ScopedEnvVar() { set_env_var(name, previous); }
};

std::filesystem::path temp_path(std::string_view prefix) {
  return std::filesystem::temp_directory_path() /
         (std::string{prefix} + "-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

std::unique_ptr<RunningServer> start_server(std::filesystem::path data_dir = {}) {
  auto running = std::make_unique<RunningServer>();
  if (data_dir.empty()) {
    running->data_dir = temp_path("boltstream-phase4-test");
    running->remove_data_dir = true;
  } else {
    running->data_dir = std::move(data_dir);
    running->remove_data_dir = false;
  }

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

std::vector<std::uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }

std::string text(const std::vector<std::uint8_t>& bytes) { return {bytes.begin(), bytes.end()}; }

boltstream::protocol::ErrorResponse decode_error(const boltstream::protocol::Frame& frame) {
  boltstream::protocol::ErrorResponse response;
  const auto decoded = boltstream::protocol::decode_error_response(frame.payload, response);
  EXPECT_TRUE(decoded.ok) << decoded.message;
  return response;
}

using RequestStarter = std::function<void(
    boltstream::client::AsyncClient&,
    std::function<void(boost::system::error_code, boltstream::protocol::Frame)>)>;

boltstream::protocol::Frame run_single_request(std::uint16_t port, RequestStarter starter,
                                               boost::system::error_code& seen_error,
                                               bool& timed_out) {
  boost::asio::io_context io;
  boltstream::client::AsyncClient client{io};
  boost::asio::steady_timer timer{io};
  boltstream::protocol::Frame seen_frame;
  bool completed = false;
  timed_out = false;

  auto finish = [&](boost::system::error_code ec, boltstream::protocol::Frame frame) {
    if (completed) {
      return;
    }
    completed = true;
    seen_error = ec;
    seen_frame = std::move(frame);
    timer.cancel();
    client.close();
    io.stop();
  };

  timer.expires_after(std::chrono::seconds(3));
  timer.async_wait([&](const boost::system::error_code& ec) {
    if (!ec) {
      timed_out = true;
      finish(make_error_code(boost::asio::error::timed_out), {});
    }
  });

  client.async_connect("127.0.0.1", port, [&](const boost::system::error_code& ec) {
    if (ec) {
      finish(ec, {});
      return;
    }
    starter(client, finish);
  });

  io.run();
  return seen_frame;
}

boltstream::protocol::Frame produce(std::uint16_t port, std::string_view topic,
                                    std::string_view key, std::string_view message,
                                    boost::system::error_code& seen_error, bool& timed_out) {
  const auto key_bytes = bytes(key);
  const auto message_bytes = bytes(message);
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_produce(topic, key_bytes, message_bytes, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame fetch(std::uint16_t port, std::string_view topic, std::string_view from,
                                  boost::system::error_code& seen_error, bool& timed_out) {
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_fetch(topic, from, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame metadata(std::uint16_t port, boost::system::error_code& seen_error,
                                     bool& timed_out) {
  return run_single_request(
      port,
      [](boltstream::client::AsyncClient& client,
         std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_metadata(std::move(done));
      },
      seen_error, timed_out);
}

} // namespace

TEST(ClientBrokerTests, HealthRequestReturnsReady) {
  const auto running = start_server();
  boost::system::error_code seen_error;
  bool timed_out = false;
  auto frame = run_single_request(
      running->port,
      [](boltstream::client::AsyncClient& client,
         std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_health(std::move(done));
      },
      seen_error, timed_out);

  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(frame.header.frame_type, boltstream::protocol::FrameType::HealthResponse);

  boltstream::protocol::HealthResponse response;
  const auto decoded = boltstream::protocol::decode_health_response(frame.payload, response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(response.status, "ready");
}

TEST(ClientBrokerTests, ProduceFetchAndMetadataRoundTrip) {
  const auto running = start_server();
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto produced =
      produce(running->port, "trades", "AAPL", "AAPL,100,192.41", seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(produced.header.frame_type, boltstream::protocol::FrameType::ProduceResponse);

  boltstream::protocol::ProduceResponse produce_response;
  auto decoded = boltstream::protocol::decode_produce_response(produced.payload, produce_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(produce_response.topic, "trades");
  EXPECT_EQ(produce_response.partition, 0U);
  EXPECT_EQ(produce_response.offset, 0U);
  EXPECT_EQ(produce_response.next_offset, 1U);

  auto fetched = fetch(running->port, "trades", "beginning", seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(fetched.header.frame_type, boltstream::protocol::FrameType::FetchResponse);

  boltstream::protocol::FetchResponse fetch_response;
  decoded = boltstream::protocol::decode_fetch_response(fetched.payload, fetch_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(fetch_response.records.size(), 1U);
  EXPECT_EQ(fetch_response.from_offset, 0U);
  EXPECT_EQ(fetch_response.next_offset, 1U);
  EXPECT_EQ(fetch_response.records[0].offset, 0U);
  EXPECT_EQ(text(fetch_response.records[0].key), "AAPL");
  EXPECT_EQ(text(fetch_response.records[0].message), "AAPL,100,192.41");

  auto latest = fetch(running->port, "trades", "latest", seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  decoded = boltstream::protocol::decode_fetch_response(latest.payload, fetch_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(fetch_response.from_offset, 1U);
  EXPECT_TRUE(fetch_response.records.empty());

  auto meta = metadata(running->port, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(meta.header.frame_type, boltstream::protocol::FrameType::MetadataResponse);
  boltstream::protocol::MetadataResponse metadata_response;
  decoded = boltstream::protocol::decode_metadata_response(meta.payload, metadata_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(metadata_response.topics.size(), 1U);
  EXPECT_EQ(metadata_response.topics[0].topic, "trades");
  EXPECT_EQ(metadata_response.topics[0].next_offset, 1U);
}

TEST(ClientBrokerTests, ProduceSurvivesBrokerRestart) {
  const auto data_dir = temp_path("boltstream-phase4-persist");
  boost::system::error_code seen_error;
  bool timed_out = false;

  {
    const auto running = start_server(data_dir);
    auto produced =
        produce(running->port, "trades", "MSFT", "MSFT,200,401.50", seen_error, timed_out);
    ASSERT_FALSE(timed_out);
    ASSERT_FALSE(seen_error) << seen_error.message();
    ASSERT_EQ(produced.header.frame_type, boltstream::protocol::FrameType::ProduceResponse);
  }

  {
    const auto running = start_server(data_dir);
    auto fetched = fetch(running->port, "trades", "0", seen_error, timed_out);
    ASSERT_FALSE(timed_out);
    ASSERT_FALSE(seen_error) << seen_error.message();
    boltstream::protocol::FetchResponse response;
    const auto decoded = boltstream::protocol::decode_fetch_response(fetched.payload, response);
    ASSERT_TRUE(decoded.ok) << decoded.message;
    ASSERT_EQ(response.records.size(), 1U);
    EXPECT_EQ(text(response.records[0].key), "MSFT");
    EXPECT_EQ(text(response.records[0].message), "MSFT,200,401.50");
    EXPECT_EQ(response.next_offset, 1U);
  }

  std::error_code ignored;
  std::filesystem::remove_all(data_dir, ignored);
}

TEST(ClientBrokerTests, InvalidTopicIsRejected) {
  const auto running = start_server();
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto frame = produce(running->port, "../bad", "AAPL", "100", seen_error, timed_out);

  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(frame.header.frame_type, boltstream::protocol::FrameType::ErrorResponse);
  const auto response = decode_error(frame);
  EXPECT_EQ(response.code, boltstream::protocol::ErrorCode::MalformedPayload);
}

TEST(ClientBrokerTests, ConfiguredAuthIsRequiredAndAccepted) {
  ScopedEnvVar token{"BOLTSTREAM_BROKER_TOKEN", std::string{"secret-token"}};
  const auto running = start_server();
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto unauthorized = produce(running->port, "trades", "AAPL", "100", seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(unauthorized.header.frame_type, boltstream::protocol::FrameType::ErrorResponse);
  EXPECT_EQ(decode_error(unauthorized).code, boltstream::protocol::ErrorCode::Unauthorized);

  boost::asio::io_context io;
  auto client = std::make_shared<boltstream::client::AsyncClient>(io);
  boost::asio::steady_timer timer{io};
  boltstream::protocol::Frame produce_frame;
  bool completed = false;
  timed_out = false;
  seen_error = {};

  auto finish = [&](boost::system::error_code ec, boltstream::protocol::Frame frame) {
    if (completed) {
      return;
    }
    completed = true;
    seen_error = ec;
    produce_frame = std::move(frame);
    timer.cancel();
    client->close();
    io.stop();
  };

  timer.expires_after(std::chrono::seconds(3));
  timer.async_wait([&](const boost::system::error_code& ec) {
    if (!ec) {
      timed_out = true;
      finish(make_error_code(boost::asio::error::timed_out), {});
    }
  });

  const auto key = bytes("AAPL");
  const auto value = bytes("100");
  client->async_connect("127.0.0.1", running->port, [&](const boost::system::error_code& ec) {
    if (ec) {
      finish(ec, {});
      return;
    }
    client->async_auth("secret-token", [&, client](const boost::system::error_code& auth_ec,
                                                   boltstream::protocol::Frame auth_frame) {
      if (auth_ec) {
        finish(auth_ec, {});
        return;
      }
      if (auth_frame.header.frame_type != boltstream::protocol::FrameType::AuthResponse) {
        finish(make_error_code(boost::asio::error::invalid_argument), std::move(auth_frame));
        return;
      }
      client->async_produce("trades", key, value, finish);
    });
  });
  io.run();

  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(produce_frame.header.frame_type, boltstream::protocol::FrameType::ProduceResponse);
}

TEST(ClientBrokerTests, ConcurrentProducersAssignUniqueOffsets) {
  const auto running = start_server();
  boost::asio::io_context io;
  boost::asio::steady_timer timer{io};
  constexpr int kProducerCount = 8;
  std::vector<std::shared_ptr<boltstream::client::AsyncClient>> clients;
  std::vector<boltstream::protocol::Frame> produce_frames;
  boltstream::protocol::Frame fetch_frame;
  boost::system::error_code seen_error;
  bool timed_out = false;
  bool fetch_started = false;

  auto stop_success = [&] {
    timer.cancel();
    for (auto& client : clients) {
      client->close();
    }
    io.stop();
  };

  auto start_fetch = [&] {
    if (fetch_started || produce_frames.size() != static_cast<std::size_t>(kProducerCount)) {
      return;
    }
    fetch_started = true;
    auto consumer = std::make_shared<boltstream::client::AsyncClient>(io);
    clients.push_back(consumer);
    consumer->async_connect("127.0.0.1", running->port,
                            [&, consumer](const boost::system::error_code& ec) {
                              if (ec) {
                                seen_error = ec;
                                io.stop();
                                return;
                              }
                              consumer->async_fetch("trades", "beginning",
                                                    [&](const boost::system::error_code& request_ec,
                                                        boltstream::protocol::Frame frame) {
                                                      seen_error = request_ec;
                                                      fetch_frame = std::move(frame);
                                                      stop_success();
                                                    });
                            });
  };

  timer.expires_after(std::chrono::seconds(5));
  timer.async_wait([&](const boost::system::error_code& ec) {
    if (!ec) {
      timed_out = true;
      for (auto& client : clients) {
        client->close();
      }
      io.stop();
    }
  });

  for (int index = 0; index < kProducerCount; ++index) {
    auto client = std::make_shared<boltstream::client::AsyncClient>(io);
    clients.push_back(client);
    const auto key = std::make_shared<std::vector<std::uint8_t>>(
        bytes(std::string{"key-"} + std::to_string(index)));
    const auto value = std::make_shared<std::vector<std::uint8_t>>(
        bytes(std::string{"value-"} + std::to_string(index)));
    client->async_connect("127.0.0.1", running->port,
                          [&, client, key, value](const boost::system::error_code& ec) {
                            if (ec) {
                              seen_error = ec;
                              io.stop();
                              return;
                            }
                            client->async_produce("trades", *key, *value,
                                                  [&](const boost::system::error_code& request_ec,
                                                      boltstream::protocol::Frame frame) {
                                                    if (request_ec) {
                                                      seen_error = request_ec;
                                                      io.stop();
                                                      return;
                                                    }
                                                    produce_frames.push_back(std::move(frame));
                                                    start_fetch();
                                                  });
                          });
  }
  io.run();

  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(produce_frames.size(), static_cast<std::size_t>(kProducerCount));
  std::vector<std::uint64_t> offsets;
  for (const auto& frame : produce_frames) {
    ASSERT_EQ(frame.header.frame_type, boltstream::protocol::FrameType::ProduceResponse);
    boltstream::protocol::ProduceResponse response;
    const auto decoded = boltstream::protocol::decode_produce_response(frame.payload, response);
    ASSERT_TRUE(decoded.ok) << decoded.message;
    offsets.push_back(response.offset);
  }
  std::sort(offsets.begin(), offsets.end());
  for (std::uint64_t expected = 0; expected < offsets.size(); ++expected) {
    EXPECT_EQ(offsets[expected], expected);
  }

  ASSERT_EQ(fetch_frame.header.frame_type, boltstream::protocol::FrameType::FetchResponse);
  boltstream::protocol::FetchResponse fetch_response;
  const auto decoded =
      boltstream::protocol::decode_fetch_response(fetch_frame.payload, fetch_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(fetch_response.records.size(), static_cast<std::size_t>(kProducerCount));
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
  EXPECT_EQ(frames[0].header.frame_type, boltstream::protocol::FrameType::HealthResponse);
  EXPECT_EQ(frames[1].header.frame_type, boltstream::protocol::FrameType::MetadataResponse);
  EXPECT_EQ(frames[2].header.frame_type, boltstream::protocol::FrameType::FetchResponse);
}
