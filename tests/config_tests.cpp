#include "boltstream/broker/options.h"
#include "boltstream/config/config.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

class TemporaryConfig {
public:
  explicit TemporaryConfig(std::string_view content) {
    static std::uint64_t next_id = 1;
    path_ = std::filesystem::temp_directory_path() /
            ("boltstream-config-test-" + std::to_string(next_id++) + ".yaml");
    std::ofstream out{path_, std::ios::binary | std::ios::trunc};
    out << content;
  }

  ~TemporaryConfig() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace

TEST(ConfigTests, LoadsYamlAndAppliesCliOverrides) {
  TemporaryConfig config{R"(server:
  listen: "127.0.0.1:9200"
  admin_listen: "127.0.0.1:9300"
storage:
  data_dir: "yaml-data"
  segment_bytes: 512
  segment_max_age_seconds: 0
retention:
  max_age_seconds: 0
  max_bytes: 2048
  check_interval_ms: 0
limits:
  max_frame_bytes: 4096
  max_fetch_records: 12
  max_fetch_bytes: 2048
  max_topic_partitions: 8
  max_fetch_wait_ms: 500
  max_append_queue_depth: 0
  append_workers: 4
  max_broker_connections: 16
  max_long_poll_waiters: 0
metrics:
  enabled: false
logging:
  level: debug
  format: json
)"};
  const auto path = config.path().string();
  const std::array args{std::string_view{"--config"},
                        std::string_view{path},
                        std::string_view{"--port"},
                        std::string_view{"9400"},
                        std::string_view{"--metrics-enabled"},
                        std::string_view{"true"},
                        std::string_view{"--log-level"},
                        std::string_view{"warn"}};

  const auto parsed = boltstream::broker::parse_server_options(args);

  ASSERT_TRUE(parsed.ok()) << parsed.error;
  EXPECT_TRUE(parsed.config_loaded);
  EXPECT_EQ(parsed.options.listen.host, "127.0.0.1");
  EXPECT_EQ(parsed.options.listen.port, 9400);
  EXPECT_EQ(parsed.options.admin_listen.port, 9300);
  EXPECT_EQ(parsed.options.data_dir.generic_string(), "yaml-data");
  EXPECT_EQ(parsed.options.segment_bytes, 512U);
  EXPECT_EQ(parsed.options.max_append_queue_depth, 0U);
  EXPECT_EQ(parsed.options.append_workers, 4U);
  EXPECT_TRUE(parsed.options.metrics_enabled);
  EXPECT_EQ(parsed.options.log_level, "warn");
}

TEST(ConfigTests, RejectsUnknownAndDuplicateKeysWithLocations) {
  TemporaryConfig unknown{R"(server:
  listen: "127.0.0.1:9000"
  mystery: 1
)"};
  const auto unknown_path = unknown.path().string();
  const std::array unknown_args{std::string_view{"--config"}, std::string_view{unknown_path}};
  const auto unknown_result = boltstream::broker::parse_server_options(unknown_args);
  EXPECT_FALSE(unknown_result.ok());
  EXPECT_NE(unknown_result.error.find("server.mystery"), std::string::npos);
  EXPECT_NE(unknown_result.error.find("line"), std::string::npos);

  TemporaryConfig duplicate{R"(metrics:
  enabled: true
  enabled: false
)"};
  const auto duplicate_path = duplicate.path().string();
  const std::array duplicate_args{std::string_view{"--config"}, std::string_view{duplicate_path}};
  const auto duplicate_result = boltstream::broker::parse_server_options(duplicate_args);
  EXPECT_FALSE(duplicate_result.ok());
  EXPECT_NE(duplicate_result.error.find("duplicate configuration key metrics.enabled"),
            std::string::npos);
}

TEST(ConfigTests, RejectsInvalidTypesRangesAndLogging) {
  TemporaryConfig config{R"(limits:
  append_workers: 0
)"};
  boltstream::broker::ServerOptions options;
  const auto result = boltstream::config::load_server_config(config.path(), options);
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("limits.append_workers"), std::string::npos);

  TemporaryConfig logging{R"(logging:
  level: verbose
  format: text
)"};
  const auto logging_result = boltstream::config::load_server_config(logging.path(), options);
  EXPECT_FALSE(logging_result.ok);
  EXPECT_NE(logging_result.error.find("logging.level"), std::string::npos);
}

TEST(ConfigTests, EffectiveConfigIsDeterministicAndSecretFree) {
  boltstream::broker::ServerOptions options;
  options.listen = {"127.0.0.1", 9900};
  options.log_level = "warn";

  const auto first = boltstream::config::effective_config_yaml(options, true);
  const auto second = boltstream::config::effective_config_yaml(options, true);

  EXPECT_EQ(first, second);
  EXPECT_NE(first.find("listen: \"127.0.0.1:9900\""), std::string::npos);
  EXPECT_NE(first.find("auth_required: true"), std::string::npos);
  EXPECT_EQ(first.find("token"), std::string::npos);
  EXPECT_EQ(first.find("secret"), std::string::npos);
}
