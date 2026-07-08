#include "boltstream/build_info.h"

#include <gtest/gtest.h>

TEST(BuildInfoTests, CurrentBuildInfoContainsProtocolAndStorageVersions) {
  const auto info = boltstream::current_build_info();

  EXPECT_EQ(info.service, "boltstream");
  EXPECT_FALSE(info.version.empty());
  EXPECT_FALSE(info.git_sha.empty());
  EXPECT_EQ(info.protocol_version, "1");
  EXPECT_EQ(info.storage_format_version, "1");
}

TEST(BuildInfoTests, JsonContainsExpectedKeys) {
  const auto json =
      boltstream::build_info_json(boltstream::current_build_info(), "2026-07-07T00:00:00Z");

  EXPECT_NE(json.find("\"service\":\"boltstream\""), std::string::npos);
  EXPECT_NE(json.find("\"protocol_version\":\"1\""), std::string::npos);
  EXPECT_NE(json.find("\"storage_format_version\":\"1\""), std::string::npos);
  EXPECT_NE(json.find("\"startup_time_utc\":\"2026-07-07T00:00:00Z\""), std::string::npos);
}
