#include "boltstream/observability/metrics.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

using boltstream::observability::MetricsRegistry;
using boltstream::protocol::ErrorCode;
using boltstream::protocol::FrameType;

TEST(MetricsTests, RegistryRecordsCountersErrorsHistogramsAndRecovery) {
  MetricsRegistry metrics;
  metrics.record_request(FrameType::ProduceRequest, 64);
  metrics.record_response(FrameType::ProduceRequest, 96, 0.004);
  metrics.record_error(FrameType::ProduceRequest, ErrorCode::Overloaded);
  metrics.record_rejection(FrameType::ProduceRequest,
                           boltstream::observability::RejectionReason::AppendQueue);
  metrics.record_connection_accepted();
  metrics.record_connection_rejected();
  metrics.record_records_produced(3);
  metrics.record_records_fetched(2);
  metrics.record_group_rebalance();
  metrics.record_retention_run(2, 128);
  metrics.set_recovery(0.25, 10, 4);
  metrics.record_metrics_scrape();
  metrics.record_metrics_render_duration(0.001);

  const auto snapshot = metrics.snapshot();
  const auto produce = static_cast<std::size_t>(FrameType::ProduceRequest);
  const auto overloaded = static_cast<std::size_t>(ErrorCode::Overloaded);
  EXPECT_EQ(snapshot.requests[produce], 1U);
  EXPECT_EQ(snapshot.errors[produce][overloaded], 1U);
  EXPECT_EQ(snapshot.request_durations[produce].count, 1U);
  EXPECT_DOUBLE_EQ(snapshot.request_durations[produce].sum, 0.004);
  EXPECT_EQ(snapshot.connections_accepted, 1U);
  EXPECT_EQ(snapshot.connections_rejected_limit, 1U);
  EXPECT_EQ(snapshot.protocol_received_bytes, 64U);
  EXPECT_EQ(snapshot.protocol_sent_bytes, 96U);
  EXPECT_EQ(snapshot.records_produced, 3U);
  EXPECT_EQ(snapshot.records_fetched, 2U);
  EXPECT_EQ(snapshot.group_rebalances, 1U);
  EXPECT_EQ(snapshot.retention_runs, 1U);
  EXPECT_EQ(snapshot.retention_deleted_segments, 2U);
  EXPECT_EQ(snapshot.retention_deleted_bytes, 128U);
  EXPECT_DOUBLE_EQ(snapshot.recovery_duration_seconds, 0.25);
  EXPECT_EQ(snapshot.recovered_records, 10U);
  EXPECT_EQ(snapshot.truncated_bytes, 4U);
  EXPECT_EQ(snapshot.metrics_scrapes, 1U);
  EXPECT_EQ(snapshot.metrics_render_duration.count, 1U);
}

TEST(MetricsTests, ConcurrentCounterUpdatesAreExact) {
  MetricsRegistry metrics;
  std::vector<std::thread> threads;
  for (std::size_t thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&metrics] {
      for (std::size_t index = 0; index < 1000; ++index) {
        metrics.record_request(FrameType::FetchRequest, 32);
        metrics.record_response(FrameType::FetchRequest, 48, 0.001);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  const auto snapshot = metrics.snapshot();
  const auto fetch = static_cast<std::size_t>(FrameType::FetchRequest);
  EXPECT_EQ(snapshot.requests[fetch], 4000U);
  EXPECT_EQ(snapshot.request_durations[fetch].count, 4000U);
  EXPECT_EQ(snapshot.protocol_received_bytes, 128000U);
  EXPECT_EQ(snapshot.protocol_sent_bytes, 192000U);
}

TEST(MetricsTests, RendererEscapesLabelsAndEmitsCumulativeHistograms) {
  MetricsRegistry metrics;
  metrics.record_request(FrameType::FetchRequest, 32);
  metrics.record_response(FrameType::FetchRequest, 64, 0.004);

  boltstream::observability::RuntimeMetricsSnapshot snapshot;
  snapshot.build_info = {"boltstream", "0.1.0", "abc123", "Debug", "GCC \"test\"", "4", "2"};
  snapshot.registry = metrics.snapshot();
  snapshot.ready = true;
  snapshot.partitions.push_back({"quotes\\live", 2, 1, 32, 3, 4096, 10, 20});
  snapshot.groups.push_back({"dash\nboard", "quotes\\live", 1, 4});
  snapshot.lags.push_back({"dash\nboard", "quotes\\live", 2, 5, false});

  const auto output = boltstream::observability::render_prometheus(snapshot);

  EXPECT_NE(output.find("compiler=\"GCC \\\"test\\\"\""), std::string::npos);
  EXPECT_NE(output.find("topic=\"quotes\\\\live\""), std::string::npos);
  EXPECT_NE(output.find("group=\"dash\\nboard\""), std::string::npos);
  EXPECT_NE(
      output.find("boltstream_request_duration_seconds_bucket{operation=\"fetch\",le=\"0.005\"} 1"),
      std::string::npos);
  EXPECT_NE(
      output.find("boltstream_request_duration_seconds_bucket{operation=\"fetch\",le=\"+Inf\"} 1"),
      std::string::npos);
  EXPECT_EQ(output.back(), '\n');
}

TEST(MetricsTests, DisabledRegistryRemainsZero) {
  MetricsRegistry metrics{false};
  metrics.record_request(FrameType::ProduceRequest, 64);
  metrics.record_records_produced(1);
  metrics.record_retention_run(1, 10);

  const auto snapshot = metrics.snapshot();
  EXPECT_EQ(snapshot.requests[static_cast<std::size_t>(FrameType::ProduceRequest)], 0U);
  EXPECT_EQ(snapshot.records_produced, 0U);
  EXPECT_EQ(snapshot.retention_runs, 0U);
}
