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
