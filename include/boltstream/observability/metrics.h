#pragma once

#include "boltstream/build_info.h"
#include "boltstream/protocol/protocol.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace boltstream::observability {

inline constexpr std::size_t kMetricFrameTypeCount = 38;
inline constexpr std::size_t kMetricErrorCodeCount = 21;
inline constexpr std::array<double, 16> kRequestDurationBuckets{
    0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1,
    0.25,   0.5,   1.0,    2.5,   5.0,  10.0,  30.0, 60.0};
inline constexpr std::array<std::uint64_t, 11> kAppendBatchRecordBuckets{1,  2,   4,   8,   16,  32,
                                                                         64, 128, 256, 512, 1024};

enum class RejectionReason : std::uint8_t {
  AppendQueue = 0,
  LongPollLimit = 1,
  ConnectionLimit = 2,
  Count = 3
};

struct HistogramSnapshot {
  std::array<std::uint64_t, kRequestDurationBuckets.size()> buckets{};
  std::uint64_t count{0};
  double sum{0.0};
};

struct AppendBatchHistogramSnapshot {
  std::array<std::uint64_t, kAppendBatchRecordBuckets.size()> buckets{};
  std::uint64_t count{0};
  std::uint64_t sum{0};
};

struct RegistrySnapshot {
  std::array<std::uint64_t, kMetricFrameTypeCount> requests{};
  std::array<std::array<std::uint64_t, kMetricErrorCodeCount>, kMetricFrameTypeCount> errors{};
  std::array<HistogramSnapshot, kMetricFrameTypeCount> request_durations{};
  std::array<std::array<std::uint64_t, static_cast<std::size_t>(RejectionReason::Count)>,
             kMetricFrameTypeCount>
      rejections{};
  std::uint64_t connections_accepted{0};
  std::uint64_t connections_rejected_limit{0};
  std::uint64_t protocol_received_bytes{0};
  std::uint64_t protocol_sent_bytes{0};
  std::uint64_t records_produced{0};
  std::uint64_t records_fetched{0};
  std::uint64_t append_batches{0};
  AppendBatchHistogramSnapshot append_batch_records;
  std::uint64_t group_rebalances{0};
  std::uint64_t group_heartbeat_failures{0};
  std::uint64_t group_commit_failures{0};
  std::uint64_t retention_runs{0};
  std::uint64_t retention_failures{0};
  std::uint64_t retention_deleted_segments{0};
  std::uint64_t retention_deleted_bytes{0};
  std::uint64_t metrics_scrapes{0};
  HistogramSnapshot metrics_render_duration;
  std::uint64_t metrics_render_failures{0};
  double recovery_duration_seconds{0.0};
  std::uint64_t recovered_records{0};
  std::uint64_t truncated_bytes{0};
};

struct PartitionMetrics {
  std::string topic;
  std::uint16_t partition{0};
  std::uint64_t append_queue_depth{0};
  std::uint64_t append_queue_capacity{0};
  std::uint64_t segments{0};
  std::uint64_t log_bytes{0};
  std::uint64_t earliest_offset{0};
  std::uint64_t next_offset{0};
};

struct ConsumerGroupMetrics {
  std::string group;
  std::string topic;
  std::uint64_t members{0};
  std::uint64_t generation{0};
};

struct ConsumerLagMetrics {
  std::string group;
  std::string topic;
  std::uint16_t partition{0};
  std::uint64_t lag{0};
  bool offset_out_of_range{false};
};

struct RuntimeMetricsSnapshot {
  BuildInfo build_info;
  RegistrySnapshot registry;
  double uptime_seconds{0.0};
  bool ready{false};
  std::uint64_t connections_active{0};
  std::uint64_t long_poll_waiters{0};
  std::uint64_t topic_count{0};
  std::uint64_t partition_count{0};
  std::uint64_t storage_capacity_bytes{0};
  std::uint64_t storage_available_bytes{0};
  std::uint64_t io_workers{0};
  std::uint64_t append_workers{0};
  std::vector<PartitionMetrics> partitions;
  std::vector<ConsumerGroupMetrics> groups;
  std::vector<ConsumerLagMetrics> lags;
};

class MetricsRegistry {
public:
  explicit MetricsRegistry(bool enabled = true) : enabled_(enabled) {}

  [[nodiscard]] bool enabled() const { return enabled_; }
  void record_request(protocol::FrameType operation, std::uint64_t received_bytes);
  void record_response(protocol::FrameType operation, std::uint64_t sent_bytes,
                       double duration_seconds);
  void record_error(protocol::FrameType operation, protocol::ErrorCode error);
  void record_rejection(protocol::FrameType operation, RejectionReason reason);
  void record_connection_accepted();
  void record_connection_rejected();
  void record_records_produced(std::uint64_t count);
  void record_records_fetched(std::uint64_t count);
  void record_append_batch(std::uint64_t records);
  void record_group_rebalance();
  void record_retention_run(std::uint64_t segments_deleted, std::uint64_t bytes_deleted);
  void record_retention_failure();
  void set_recovery(double duration_seconds, std::uint64_t records, std::uint64_t truncated_bytes);
  void record_metrics_scrape();
  void record_metrics_render_duration(double duration_seconds);
  void record_metrics_render_failure();
  [[nodiscard]] RegistrySnapshot snapshot() const;

private:
  class Histogram {
  public:
    void observe(double value);
    [[nodiscard]] HistogramSnapshot snapshot() const;

  private:
    mutable std::mutex mutex_;
    std::array<std::uint64_t, kRequestDurationBuckets.size()> buckets_{};
    std::uint64_t count_{0};
    double sum_{0.0};
  };

  class AppendBatchHistogram {
  public:
    void observe(std::uint64_t value);
    [[nodiscard]] AppendBatchHistogramSnapshot snapshot() const;

  private:
    mutable std::mutex mutex_;
    std::array<std::uint64_t, kAppendBatchRecordBuckets.size()> buckets_{};
    std::uint64_t count_{0};
    std::uint64_t sum_{0};
  };

  [[nodiscard]] static std::size_t frame_index(protocol::FrameType operation);
  [[nodiscard]] static std::size_t error_index(protocol::ErrorCode error);

  bool enabled_{true};
  std::array<std::atomic<std::uint64_t>, kMetricFrameTypeCount> requests_{};
  std::array<std::array<std::atomic<std::uint64_t>, kMetricErrorCodeCount>, kMetricFrameTypeCount>
      errors_{};
  std::array<Histogram, kMetricFrameTypeCount> request_durations_;
  std::array<
      std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(RejectionReason::Count)>,
      kMetricFrameTypeCount>
      rejections_{};
  std::atomic<std::uint64_t> connections_accepted_{0};
  std::atomic<std::uint64_t> connections_rejected_limit_{0};
  std::atomic<std::uint64_t> protocol_received_bytes_{0};
  std::atomic<std::uint64_t> protocol_sent_bytes_{0};
  std::atomic<std::uint64_t> records_produced_{0};
  std::atomic<std::uint64_t> records_fetched_{0};
  std::atomic<std::uint64_t> append_batches_{0};
  AppendBatchHistogram append_batch_records_;
  std::atomic<std::uint64_t> group_rebalances_{0};
  std::atomic<std::uint64_t> group_heartbeat_failures_{0};
  std::atomic<std::uint64_t> group_commit_failures_{0};
  std::atomic<std::uint64_t> retention_runs_{0};
  std::atomic<std::uint64_t> retention_failures_{0};
  std::atomic<std::uint64_t> retention_deleted_segments_{0};
  std::atomic<std::uint64_t> retention_deleted_bytes_{0};
  std::atomic<std::uint64_t> metrics_scrapes_{0};
  Histogram metrics_render_duration_;
  std::atomic<std::uint64_t> metrics_render_failures_{0};
  mutable std::mutex recovery_mutex_;
  double recovery_duration_seconds_{0.0};
  std::uint64_t recovered_records_{0};
  std::uint64_t truncated_bytes_{0};
};

std::string render_prometheus(const RuntimeMetricsSnapshot& snapshot);
std::string prometheus_label_escape(std::string_view value);
std::string_view rejection_reason_name(RejectionReason reason);

} // namespace boltstream::observability
