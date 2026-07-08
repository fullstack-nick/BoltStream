#include "boltstream/broker/options.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

using boltstream::broker::parse_server_options;

TEST(OptionsTests, DefaultsMatchCurrentContract) {
  constexpr std::array<std::string_view, 0> args{};

  const auto parsed = parse_server_options(args);

  ASSERT_TRUE(parsed.ok()) << parsed.error;
  EXPECT_EQ(parsed.options.listen.host, "0.0.0.0");
  EXPECT_EQ(parsed.options.listen.port, 9000);
  EXPECT_EQ(parsed.options.admin_listen.host, "127.0.0.1");
  EXPECT_EQ(parsed.options.admin_listen.port, 9100);
  EXPECT_EQ(parsed.options.data_dir.generic_string(), "./data");
  EXPECT_EQ(parsed.options.max_frame_bytes, 1024U * 1024U);
  EXPECT_EQ(parsed.options.max_fetch_records, 100U);
  EXPECT_EQ(parsed.options.max_fetch_bytes, 1024U * 1024U);
  EXPECT_EQ(parsed.options.max_topic_partitions, 128U);
  EXPECT_EQ(parsed.options.max_fetch_wait_ms, 30000U);
  EXPECT_EQ(parsed.options.max_append_queue_depth, 32U);
  EXPECT_EQ(parsed.options.append_workers, 2U);
  EXPECT_EQ(parsed.options.max_broker_connections, 128U);
  EXPECT_EQ(parsed.options.max_long_poll_waiters, 128U);
  EXPECT_EQ(parsed.options.segment_bytes, 256ULL * 1024ULL * 1024ULL);
  EXPECT_EQ(parsed.options.segment_max_age_seconds, 3600U);
  EXPECT_EQ(parsed.options.retention_max_age_seconds, 604800U);
  EXPECT_EQ(parsed.options.retention_max_bytes, 1024ULL * 1024ULL * 1024ULL);
  EXPECT_EQ(parsed.options.retention_check_interval_ms, 60000U);
}

TEST(OptionsTests, ParsesListenAdminPortDataAndLimits) {
  constexpr std::array args{
      std::string_view{"--listen"},
      std::string_view{"127.0.0.1:9001"},
      std::string_view{"--admin-listen"},
      std::string_view{"127.0.0.1:9101"},
      std::string_view{"--data"},
      std::string_view{"./tmp-data"},
      std::string_view{"--max-frame-bytes"},
      std::string_view{"4096"},
      std::string_view{"--max-fetch-records"},
      std::string_view{"12"},
      std::string_view{"--max-fetch-bytes"},
      std::string_view{"2048"},
      std::string_view{"--max-topic-partitions"},
      std::string_view{"8"},
      std::string_view{"--max-fetch-wait-ms"},
      std::string_view{"500"},
      std::string_view{"--max-append-queue-depth"},
      std::string_view{"0"},
      std::string_view{"--append-workers"},
      std::string_view{"4"},
      std::string_view{"--max-broker-connections"},
      std::string_view{"16"},
      std::string_view{"--max-long-poll-waiters"},
      std::string_view{"0"},
      std::string_view{"--segment-bytes"},
      std::string_view{"512"},
      std::string_view{"--segment-max-age-seconds"},
      std::string_view{"0"},
      std::string_view{"--retention-max-age-seconds"},
      std::string_view{"0"},
      std::string_view{"--retention-max-bytes"},
      std::string_view{"4096"},
      std::string_view{"--retention-check-interval-ms"},
      std::string_view{"0"},
  };

  const auto parsed = parse_server_options(args);

  ASSERT_TRUE(parsed.ok()) << parsed.error;
  EXPECT_EQ(parsed.options.listen.host, "127.0.0.1");
  EXPECT_EQ(parsed.options.listen.port, 9001);
  EXPECT_EQ(parsed.options.admin_listen.host, "127.0.0.1");
  EXPECT_EQ(parsed.options.admin_listen.port, 9101);
  EXPECT_EQ(parsed.options.data_dir.generic_string(), "./tmp-data");
  EXPECT_EQ(parsed.options.max_frame_bytes, 4096U);
  EXPECT_EQ(parsed.options.max_fetch_records, 12U);
  EXPECT_EQ(parsed.options.max_fetch_bytes, 2048U);
  EXPECT_EQ(parsed.options.max_topic_partitions, 8U);
  EXPECT_EQ(parsed.options.max_fetch_wait_ms, 500U);
  EXPECT_EQ(parsed.options.max_append_queue_depth, 0U);
  EXPECT_EQ(parsed.options.append_workers, 4U);
  EXPECT_EQ(parsed.options.max_broker_connections, 16U);
  EXPECT_EQ(parsed.options.max_long_poll_waiters, 0U);
  EXPECT_EQ(parsed.options.segment_bytes, 512U);
  EXPECT_EQ(parsed.options.segment_max_age_seconds, 0U);
  EXPECT_EQ(parsed.options.retention_max_age_seconds, 0U);
  EXPECT_EQ(parsed.options.retention_max_bytes, 4096U);
  EXPECT_EQ(parsed.options.retention_check_interval_ms, 0U);
}

TEST(OptionsTests, ParsesPortShortcut) {
  constexpr std::array args{std::string_view{"--port"}, std::string_view{"9900"}};

  const auto parsed = parse_server_options(args);

  ASSERT_TRUE(parsed.ok()) << parsed.error;
  EXPECT_EQ(parsed.options.listen.host, "0.0.0.0");
  EXPECT_EQ(parsed.options.listen.port, 9900);
}

TEST(OptionsTests, RejectsInvalidPort) {
  constexpr std::array args{std::string_view{"--port"}, std::string_view{"70000"}};

  const auto parsed = parse_server_options(args);

  EXPECT_FALSE(parsed.ok());
  EXPECT_NE(parsed.error.find("invalid --port"), std::string::npos);
}

TEST(OptionsTests, RejectsZeroAppendWorkers) {
  constexpr std::array args{std::string_view{"--append-workers"}, std::string_view{"0"}};

  const auto parsed = parse_server_options(args);

  EXPECT_FALSE(parsed.ok());
  EXPECT_NE(parsed.error.find("invalid --append-workers"), std::string::npos);
}

TEST(OptionsTests, UsageDocumentsPhaseSixDefaults) {
  const auto usage = boltstream::broker::server_usage();

  EXPECT_NE(usage.find("--max-append-queue-depth 32"), std::string::npos);
  EXPECT_NE(usage.find("--append-workers 2"), std::string::npos);
  EXPECT_NE(usage.find("--max-broker-connections 128"), std::string::npos);
  EXPECT_NE(usage.find("--max-long-poll-waiters 128"), std::string::npos);
  EXPECT_NE(usage.find("--segment-bytes 268435456"), std::string::npos);
  EXPECT_NE(usage.find("--retention-max-age-seconds 604800"), std::string::npos);
  EXPECT_NE(usage.find("--retention-max-bytes 1073741824"), std::string::npos);
}
