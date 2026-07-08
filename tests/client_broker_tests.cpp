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

std::unique_ptr<RunningServer>
start_server(std::filesystem::path data_dir = {},
             std::function<void(boltstream::broker::ServerOptions&)> configure = {}) {
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
  if (configure) {
    configure(options);
  }

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
        client.async_fetch(topic, 0, from, "", 0, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame fetch_partition(std::uint16_t port, std::string_view topic,
                                            std::uint16_t partition, std::string_view from,
                                            std::string_view group, std::uint32_t wait_ms,
                                            boost::system::error_code& seen_error,
                                            bool& timed_out) {
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_fetch(topic, partition, from, group, wait_ms, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame create_topic(std::uint16_t port, std::string_view topic,
                                         std::uint16_t partitions,
                                         boost::system::error_code& seen_error, bool& timed_out) {
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_create_topic(topic, partitions, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame commit_offset(std::uint16_t port, std::string_view group,
                                          std::string_view topic, std::uint16_t partition,
                                          std::uint64_t next_offset,
                                          boost::system::error_code& seen_error, bool& timed_out) {
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        boltstream::protocol::OffsetCommitRequest request;
        request.group = std::string{group};
        request.topic = std::string{topic};
        request.partition = partition;
        request.next_offset = next_offset;
        client.async_offset_commit(request, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame join_group(std::uint16_t port,
                                       const boltstream::protocol::JoinGroupRequest& request,
                                       boost::system::error_code& seen_error, bool& timed_out) {
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_join_group(request, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame sync_group(std::uint16_t port,
                                       const boltstream::protocol::SyncGroupRequest& request,
                                       boost::system::error_code& seen_error, bool& timed_out) {
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_sync_group(request, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame heartbeat_group(std::uint16_t port,
                                            const boltstream::protocol::HeartbeatRequest& request,
                                            boost::system::error_code& seen_error,
                                            bool& timed_out) {
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_heartbeat(request, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame leave_group(std::uint16_t port,
                                        const boltstream::protocol::LeaveGroupRequest& request,
                                        boost::system::error_code& seen_error, bool& timed_out) {
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_leave_group(request, std::move(done));
      },
      seen_error, timed_out);
}

boltstream::protocol::Frame
group_offset_commit(std::uint16_t port,
                    const boltstream::protocol::GroupOffsetCommitRequest& request,
                    boost::system::error_code& seen_error, bool& timed_out) {
  return run_single_request(
      port,
      [&](boltstream::client::AsyncClient& client,
          std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_group_offset_commit(request, std::move(done));
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

  auto created = create_topic(running->port, "trades", 1, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(created.header.frame_type, boltstream::protocol::FrameType::CreateTopicResponse);

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
    auto created = create_topic(running->port, "trades", 1, seen_error, timed_out);
    ASSERT_FALSE(timed_out);
    ASSERT_FALSE(seen_error) << seen_error.message();
    ASSERT_EQ(created.header.frame_type, boltstream::protocol::FrameType::CreateTopicResponse);

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

TEST(ClientBrokerTests, MultiPartitionTopicRoutesRecordsAndReportsMetadata) {
  const auto running = start_server();
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto created = create_topic(running->port, "multi", 3, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(created.header.frame_type, boltstream::protocol::FrameType::CreateTopicResponse);

  std::vector<std::uint16_t> round_robin_partitions;
  for (int index = 0; index < 3; ++index) {
    auto frame = produce(running->port, "multi", "", "value-" + std::to_string(index), seen_error,
                         timed_out);
    ASSERT_FALSE(timed_out);
    ASSERT_FALSE(seen_error) << seen_error.message();
    ASSERT_EQ(frame.header.frame_type, boltstream::protocol::FrameType::ProduceResponse);
    boltstream::protocol::ProduceResponse response;
    const auto decoded = boltstream::protocol::decode_produce_response(frame.payload, response);
    ASSERT_TRUE(decoded.ok) << decoded.message;
    round_robin_partitions.push_back(response.partition);
    EXPECT_EQ(response.offset, 0U);
  }
  EXPECT_EQ(round_robin_partitions, (std::vector<std::uint16_t>{0, 1, 2}));

  auto keyed_one = produce(running->port, "multi", "same-key", "keyed-1", seen_error, timed_out);
  auto keyed_two = produce(running->port, "multi", "same-key", "keyed-2", seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::ProduceResponse keyed_response_one;
  boltstream::protocol::ProduceResponse keyed_response_two;
  auto decoded =
      boltstream::protocol::decode_produce_response(keyed_one.payload, keyed_response_one);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  decoded = boltstream::protocol::decode_produce_response(keyed_two.payload, keyed_response_two);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(keyed_response_one.partition, keyed_response_two.partition);

  auto meta = metadata(running->port, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::MetadataResponse metadata_response;
  decoded = boltstream::protocol::decode_metadata_response(meta.payload, metadata_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(metadata_response.topics.size(), 3U);
  std::vector<std::uint16_t> metadata_partitions;
  for (const auto& topic : metadata_response.topics) {
    EXPECT_EQ(topic.topic, "multi");
    metadata_partitions.push_back(topic.partition);
  }
  EXPECT_EQ(metadata_partitions, (std::vector<std::uint16_t>{0, 1, 2}));
}

TEST(ClientBrokerTests, ConsumerGroupCommitSurvivesRestartAndResumes) {
  const auto data_dir = temp_path("boltstream-phase5-offsets");
  boost::system::error_code seen_error;
  bool timed_out = false;

  {
    const auto running = start_server(data_dir);
    auto created = create_topic(running->port, "trades", 1, seen_error, timed_out);
    ASSERT_FALSE(timed_out);
    ASSERT_FALSE(seen_error) << seen_error.message();
    ASSERT_EQ(created.header.frame_type, boltstream::protocol::FrameType::CreateTopicResponse);

    auto produced = produce(running->port, "trades", "AAPL", "one", seen_error, timed_out);
    ASSERT_FALSE(timed_out);
    ASSERT_FALSE(seen_error) << seen_error.message();
    ASSERT_EQ(produced.header.frame_type, boltstream::protocol::FrameType::ProduceResponse);

    auto committed =
        commit_offset(running->port, "dashboard", "trades", 0, 1, seen_error, timed_out);
    ASSERT_FALSE(timed_out);
    ASSERT_FALSE(seen_error) << seen_error.message();
    ASSERT_EQ(committed.header.frame_type, boltstream::protocol::FrameType::OffsetCommitResponse);
  }

  {
    const auto running = start_server(data_dir);
    auto fetched = fetch_partition(running->port, "trades", 0, "committed", "dashboard", 0,
                                   seen_error, timed_out);
    ASSERT_FALSE(timed_out);
    ASSERT_FALSE(seen_error) << seen_error.message();
    ASSERT_EQ(fetched.header.frame_type, boltstream::protocol::FrameType::FetchResponse);
    boltstream::protocol::FetchResponse response;
    const auto decoded = boltstream::protocol::decode_fetch_response(fetched.payload, response);
    ASSERT_TRUE(decoded.ok) << decoded.message;
    EXPECT_EQ(response.from_offset, 1U);
    EXPECT_TRUE(response.records.empty());
    EXPECT_EQ(response.next_offset, 1U);
  }

  std::error_code ignored;
  std::filesystem::remove_all(data_dir, ignored);
}

TEST(ClientBrokerTests, CoordinatedConsumerGroupsAssignPartitionsAndFenceCommits) {
  const auto running = start_server();
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto created = create_topic(running->port, "coordinated", 4, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(created.header.frame_type, boltstream::protocol::FrameType::CreateTopicResponse);

  auto joined_one =
      join_group(running->port, {"dashboard", "coordinated", "", 1000}, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(joined_one.header.frame_type, boltstream::protocol::FrameType::JoinGroupResponse);
  boltstream::protocol::JoinGroupResponse join_one_response;
  auto decoded =
      boltstream::protocol::decode_join_group_response(joined_one.payload, join_one_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(join_one_response.member_id, "member-000000000001");
  EXPECT_EQ(join_one_response.generation_id, 1U);

  auto joined_two =
      join_group(running->port, {"dashboard", "coordinated", "", 1000}, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::JoinGroupResponse join_two_response;
  decoded = boltstream::protocol::decode_join_group_response(joined_two.payload, join_two_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(join_two_response.member_id, "member-000000000002");
  EXPECT_EQ(join_two_response.generation_id, 2U);

  auto synced_one =
      sync_group(running->port, {"dashboard", "coordinated", join_one_response.member_id, 2},
                 seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::SyncGroupResponse sync_one_response;
  decoded = boltstream::protocol::decode_sync_group_response(synced_one.payload, sync_one_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(sync_one_response.assignment, (std::vector<std::uint16_t>{0, 1}));

  auto synced_two =
      sync_group(running->port, {"dashboard", "coordinated", join_two_response.member_id, 2},
                 seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::SyncGroupResponse sync_two_response;
  decoded = boltstream::protocol::decode_sync_group_response(synced_two.payload, sync_two_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(sync_two_response.assignment, (std::vector<std::uint16_t>{2, 3}));

  auto stale_commit = group_offset_commit(
      running->port, {"dashboard", "coordinated", join_one_response.member_id, 1, 0, 0}, seen_error,
      timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(stale_commit.header.frame_type, boltstream::protocol::FrameType::ErrorResponse);
  EXPECT_EQ(decode_error(stale_commit).code, boltstream::protocol::ErrorCode::StaleMember);

  auto current_commit = group_offset_commit(
      running->port, {"dashboard", "coordinated", join_one_response.member_id, 2, 0, 0}, seen_error,
      timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(current_commit.header.frame_type,
            boltstream::protocol::FrameType::GroupOffsetCommitResponse);

  auto legacy_commit =
      commit_offset(running->port, "dashboard", "coordinated", 0, 0, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(legacy_commit.header.frame_type, boltstream::protocol::FrameType::ErrorResponse);
  EXPECT_EQ(decode_error(legacy_commit).code, boltstream::protocol::ErrorCode::StaleMember);

  auto left_two =
      leave_group(running->port, {"dashboard", "coordinated", join_two_response.member_id, 2},
                  seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(left_two.header.frame_type, boltstream::protocol::FrameType::LeaveGroupResponse);

  synced_one =
      sync_group(running->port, {"dashboard", "coordinated", join_one_response.member_id, 3},
                 seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  decoded = boltstream::protocol::decode_sync_group_response(synced_one.payload, sync_one_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(sync_one_response.assignment, (std::vector<std::uint16_t>{0, 1, 2, 3}));
}

TEST(ClientBrokerTests, CoordinatedConsumerGroupTimeoutTriggersTakeover) {
  const auto running = start_server();
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto created = create_topic(running->port, "timeouts", 4, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();

  auto joined_one =
      join_group(running->port, {"dashboard", "timeouts", "", 150}, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::JoinGroupResponse join_one_response;
  auto decoded =
      boltstream::protocol::decode_join_group_response(joined_one.payload, join_one_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;

  auto joined_two =
      join_group(running->port, {"dashboard", "timeouts", "", 150}, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::JoinGroupResponse join_two_response;
  decoded = boltstream::protocol::decode_join_group_response(joined_two.payload, join_two_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(join_two_response.generation_id, 2U);

  std::this_thread::sleep_for(std::chrono::milliseconds(90));

  auto heartbeat =
      heartbeat_group(running->port, {"dashboard", "timeouts", join_one_response.member_id, 2},
                      seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(heartbeat.header.frame_type, boltstream::protocol::FrameType::HeartbeatResponse);

  std::this_thread::sleep_for(std::chrono::milliseconds(90));

  auto rebalance =
      heartbeat_group(running->port, {"dashboard", "timeouts", join_one_response.member_id, 2},
                      seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(rebalance.header.frame_type, boltstream::protocol::FrameType::ErrorResponse);
  EXPECT_EQ(decode_error(rebalance).code, boltstream::protocol::ErrorCode::RebalanceRequired);

  auto rejoined_one =
      join_group(running->port, {"dashboard", "timeouts", join_one_response.member_id, 150},
                 seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::JoinGroupResponse rejoin_response;
  decoded = boltstream::protocol::decode_join_group_response(rejoined_one.payload, rejoin_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(rejoin_response.member_id, join_one_response.member_id);
  EXPECT_EQ(rejoin_response.generation_id, 3U);

  auto synced_one =
      sync_group(running->port, {"dashboard", "timeouts", join_one_response.member_id, 3},
                 seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::SyncGroupResponse sync_response;
  decoded = boltstream::protocol::decode_sync_group_response(synced_one.payload, sync_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_EQ(sync_response.assignment, (std::vector<std::uint16_t>{0, 1, 2, 3}));
}

TEST(ClientBrokerTests, LongPollFetchCompletesAfterDelayedProduce) {
  const auto running = start_server();
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto created = create_topic(running->port, "waits", 1, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(created.header.frame_type, boltstream::protocol::FrameType::CreateTopicResponse);

  std::thread delayed_producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    boost::system::error_code produce_error;
    bool produce_timed_out = false;
    (void)produce(running->port, "waits", "AAPL", "after-wait", produce_error, produce_timed_out);
  });

  auto fetched =
      fetch_partition(running->port, "waits", 0, "latest", "", 2000, seen_error, timed_out);
  delayed_producer.join();

  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(fetched.header.frame_type, boltstream::protocol::FrameType::FetchResponse);
  boltstream::protocol::FetchResponse response;
  const auto decoded = boltstream::protocol::decode_fetch_response(fetched.payload, response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(response.records.size(), 1U);
  EXPECT_EQ(text(response.records[0].message), "after-wait");
}

TEST(ClientBrokerTests, AppendQueueDepthZeroRejectsProduceWithoutAppending) {
  const auto running = start_server({}, [](auto& options) { options.max_append_queue_depth = 0; });
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto created = create_topic(running->port, "overload", 1, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(created.header.frame_type, boltstream::protocol::FrameType::CreateTopicResponse);

  auto rejected = produce(running->port, "overload", "AAPL", "100", seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(rejected.header.frame_type, boltstream::protocol::FrameType::ErrorResponse);
  const auto error = decode_error(rejected);
  EXPECT_EQ(error.code, boltstream::protocol::ErrorCode::Overloaded);
  EXPECT_TRUE(boltstream::protocol::is_retryable_error(error.code));

  auto fetched = fetch(running->port, "overload", "beginning", seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::FetchResponse response;
  const auto decoded = boltstream::protocol::decode_fetch_response(fetched.payload, response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_TRUE(response.records.empty());
  EXPECT_EQ(response.next_offset, 0U);
}

TEST(ClientBrokerTests, LongPollWaiterLimitRejectsOnlyWaitingFetches) {
  const auto running = start_server({}, [](auto& options) { options.max_long_poll_waiters = 0; });
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto created = create_topic(running->port, "waitlimit", 1, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();

  auto waiting =
      fetch_partition(running->port, "waitlimit", 0, "latest", "", 1000, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(waiting.header.frame_type, boltstream::protocol::FrameType::ErrorResponse);
  EXPECT_EQ(decode_error(waiting).code, boltstream::protocol::ErrorCode::Overloaded);

  auto immediate =
      fetch_partition(running->port, "waitlimit", 0, "latest", "", 0, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(immediate.header.frame_type, boltstream::protocol::FrameType::FetchResponse);
  boltstream::protocol::FetchResponse response;
  const auto decoded = boltstream::protocol::decode_fetch_response(immediate.payload, response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  EXPECT_TRUE(response.records.empty());
}

TEST(ClientBrokerTests, FetchTruncationReturnsResumeNextOffset) {
  const auto running = start_server({}, [](auto& options) {
    options.max_fetch_records = 10;
    options.max_fetch_bytes = 80;
  });
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto created = create_topic(running->port, "trades", 1, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  for (std::string_view value : {"one", "two", "three"}) {
    auto produced = produce(running->port, "trades", "k", value, seen_error, timed_out);
    ASSERT_FALSE(timed_out);
    ASSERT_FALSE(seen_error) << seen_error.message();
    ASSERT_EQ(produced.header.frame_type, boltstream::protocol::FrameType::ProduceResponse);
  }

  auto first = fetch(running->port, "trades", "beginning", seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::FetchResponse first_response;
  auto decoded = boltstream::protocol::decode_fetch_response(first.payload, first_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(first_response.records.size(), 1U);
  EXPECT_EQ(first_response.records[0].offset, 0U);
  EXPECT_EQ(first_response.next_offset, 1U);

  auto second = fetch(running->port, "trades", "1", seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  boltstream::protocol::FetchResponse second_response;
  decoded = boltstream::protocol::decode_fetch_response(second.payload, second_response);
  ASSERT_TRUE(decoded.ok) << decoded.message;
  ASSERT_EQ(second_response.records.size(), 1U);
  EXPECT_EQ(second_response.records[0].offset, 1U);
  EXPECT_EQ(second_response.next_offset, 2U);
}

TEST(ClientBrokerTests, OversizedFrameReceivesInvalidLengthAndCloses) {
  const auto running = start_server({}, [](auto& options) { options.max_frame_bytes = 96; });
  boost::system::error_code seen_error;
  bool timed_out = false;

  auto created = create_topic(running->port, "frames", 1, seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();

  auto rejected =
      produce(running->port, "frames", "k", std::string(128, 'x'), seen_error, timed_out);
  ASSERT_FALSE(timed_out);
  ASSERT_EQ(rejected.header.frame_type, boltstream::protocol::FrameType::ErrorResponse);
  EXPECT_EQ(decode_error(rejected).code, boltstream::protocol::ErrorCode::InvalidLength);
}

TEST(ClientBrokerTests, BrokerConnectionLimitRejectsExcessSessions) {
  const auto running = start_server({}, [](auto& options) { options.max_broker_connections = 1; });

  boost::asio::io_context hold_io;
  boost::asio::ip::tcp::socket hold_socket{hold_io};
  hold_socket.connect({boost::asio::ip::make_address("127.0.0.1"), running->port});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  boost::system::error_code seen_error;
  bool timed_out = false;
  auto frame = run_single_request(
      running->port,
      [](boltstream::client::AsyncClient& client,
         std::function<void(boost::system::error_code, boltstream::protocol::Frame)> done) {
        client.async_health(std::move(done));
      },
      seen_error, timed_out);

  hold_socket.close();
  EXPECT_FALSE(timed_out);
  EXPECT_TRUE(seen_error ||
              frame.header.frame_type == boltstream::protocol::FrameType::ErrorResponse);
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
      client->async_create_topic("trades", 1,
                                 [&, client](const boost::system::error_code& create_ec,
                                             boltstream::protocol::Frame create_frame) {
                                   if (create_ec) {
                                     finish(create_ec, {});
                                     return;
                                   }
                                   if (create_frame.header.frame_type !=
                                       boltstream::protocol::FrameType::CreateTopicResponse) {
                                     finish(make_error_code(boost::asio::error::invalid_argument),
                                            std::move(create_frame));
                                     return;
                                   }
                                   client->async_produce("trades", key, value, finish);
                                 });
    });
  });
  io.run();

  ASSERT_FALSE(timed_out);
  ASSERT_FALSE(seen_error) << seen_error.message();
  ASSERT_EQ(produce_frame.header.frame_type, boltstream::protocol::FrameType::ProduceResponse);
}

TEST(ClientBrokerTests, ConcurrentProducersAssignUniqueOffsets) {
  const auto running = start_server();
  boost::system::error_code setup_error;
  bool setup_timed_out = false;
  auto created = create_topic(running->port, "trades", 1, setup_error, setup_timed_out);
  ASSERT_FALSE(setup_timed_out);
  ASSERT_FALSE(setup_error) << setup_error.message();
  ASSERT_EQ(created.header.frame_type, boltstream::protocol::FrameType::CreateTopicResponse);

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
                              consumer->async_fetch("trades", 0, "beginning", "", 0,
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
  boost::system::error_code setup_error;
  bool setup_timed_out = false;
  auto created = create_topic(running->port, "trades", 1, setup_error, setup_timed_out);
  ASSERT_FALSE(setup_timed_out);
  ASSERT_FALSE(setup_error) << setup_error.message();
  ASSERT_EQ(created.header.frame_type, boltstream::protocol::FrameType::CreateTopicResponse);

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
    client.async_fetch("trades", 0, "latest", "", 0, complete);
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
