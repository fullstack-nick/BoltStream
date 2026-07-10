#include "boltstream/observability/metrics.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace boltstream::observability {
namespace {

using protocol::FrameType;

constexpr std::array<FrameType, 18> kRequestOperations{
    FrameType::HealthRequest,        FrameType::MetadataRequest,
    FrameType::ProduceRequest,       FrameType::FetchRequest,
    FrameType::OffsetCommitRequest,  FrameType::AuthRequest,
    FrameType::CreateTopicRequest,   FrameType::JoinGroupRequest,
    FrameType::SyncGroupRequest,     FrameType::HeartbeatRequest,
    FrameType::LeaveGroupRequest,    FrameType::GroupOffsetCommitRequest,
    FrameType::ListTopicsRequest,    FrameType::DescribeTopicRequest,
    FrameType::DeleteTopicRequest,   FrameType::RunRetentionRequest,
    FrameType::DescribeGroupRequest, FrameType::ResetGroupOffsetRequest};

void family(std::ostringstream& out, std::string_view name, std::string_view help,
            std::string_view type) {
  out << "# HELP " << name << ' ' << help << '\n';
  out << "# TYPE " << name << ' ' << type << '\n';
}

void sample(std::ostringstream& out, std::string_view name, std::uint64_t value) {
  out << name << ' ' << value << '\n';
}

void sample(std::ostringstream& out, std::string_view name, double value) {
  out << name << ' ' << std::setprecision(17) << value << '\n';
}

std::string labels(std::initializer_list<std::pair<std::string_view, std::string>> values) {
  std::ostringstream out;
  out << '{';
  bool first = true;
  for (const auto& [name, value] : values) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << name << "=\"" << prometheus_label_escape(value) << "\"";
  }
  out << '}';
  return out.str();
}

std::string operation_name(FrameType operation) {
  auto name = std::string{protocol::frame_type_name(operation)};
  constexpr std::string_view suffix{"_request"};
  if (name.ends_with(suffix)) {
    name.resize(name.size() - suffix.size());
  }
  return name;
}

void render_histogram(std::ostringstream& out, std::string_view name, std::string_view operation,
                      const HistogramSnapshot& histogram) {
  std::uint64_t cumulative = 0;
  for (std::size_t index = 0; index < kRequestDurationBuckets.size(); ++index) {
    cumulative += histogram.buckets[index];
    std::ostringstream boundary;
    boundary << std::setprecision(10) << kRequestDurationBuckets[index];
    out << name << "_bucket"
        << labels({{"operation", std::string{operation}}, {"le", boundary.str()}}) << ' '
        << cumulative << '\n';
  }
  out << name << "_bucket" << labels({{"operation", std::string{operation}}, {"le", "+Inf"}}) << ' '
      << histogram.count << '\n';
  out << name << "_sum" << labels({{"operation", std::string{operation}}}) << ' '
      << std::setprecision(17) << histogram.sum << '\n';
  out << name << "_count" << labels({{"operation", std::string{operation}}}) << ' '
      << histogram.count << '\n';
}

void render_unlabeled_histogram(std::ostringstream& out, std::string_view name,
                                const HistogramSnapshot& histogram) {
  std::uint64_t cumulative = 0;
  for (std::size_t index = 0; index < kRequestDurationBuckets.size(); ++index) {
    cumulative += histogram.buckets[index];
    std::ostringstream boundary;
    boundary << std::setprecision(10) << kRequestDurationBuckets[index];
    out << name << "_bucket" << labels({{"le", boundary.str()}}) << ' ' << cumulative << '\n';
  }
  out << name << "_bucket" << labels({{"le", "+Inf"}}) << ' ' << histogram.count << '\n';
  out << name << "_sum " << std::setprecision(17) << histogram.sum << '\n';
  out << name << "_count " << histogram.count << '\n';
}

} // namespace

void MetricsRegistry::Histogram::observe(double value) {
  std::lock_guard lock{mutex_};
  ++count_;
  sum_ += value;
  for (std::size_t index = 0; index < kRequestDurationBuckets.size(); ++index) {
    if (value <= kRequestDurationBuckets[index]) {
      ++buckets_[index];
      break;
    }
  }
}

HistogramSnapshot MetricsRegistry::Histogram::snapshot() const {
  std::lock_guard lock{mutex_};
  return {buckets_, count_, sum_};
}

void MetricsRegistry::AppendBatchHistogram::observe(std::uint64_t value) {
  std::lock_guard lock{mutex_};
  ++count_;
  sum_ += value;
  for (std::size_t index = 0; index < kAppendBatchRecordBuckets.size(); ++index) {
    if (value <= kAppendBatchRecordBuckets[index]) {
      ++buckets_[index];
      break;
    }
  }
}

AppendBatchHistogramSnapshot MetricsRegistry::AppendBatchHistogram::snapshot() const {
  std::lock_guard lock{mutex_};
  return {buckets_, count_, sum_};
}

std::size_t MetricsRegistry::frame_index(protocol::FrameType operation) {
  return std::min<std::size_t>(static_cast<std::size_t>(operation), kMetricFrameTypeCount - 1);
}

std::size_t MetricsRegistry::error_index(protocol::ErrorCode error) {
  return std::min<std::size_t>(static_cast<std::size_t>(error), kMetricErrorCodeCount - 1);
}

void MetricsRegistry::record_request(protocol::FrameType operation, std::uint64_t received_bytes) {
  if (!enabled_) {
    return;
  }
  requests_[frame_index(operation)].fetch_add(1, std::memory_order_relaxed);
  protocol_received_bytes_.fetch_add(received_bytes, std::memory_order_relaxed);
}

void MetricsRegistry::record_response(protocol::FrameType operation, std::uint64_t sent_bytes,
                                      double duration_seconds) {
  if (!enabled_) {
    return;
  }
  protocol_sent_bytes_.fetch_add(sent_bytes, std::memory_order_relaxed);
  request_durations_[frame_index(operation)].observe(duration_seconds);
}

void MetricsRegistry::record_error(protocol::FrameType operation, protocol::ErrorCode error) {
  if (!enabled_) {
    return;
  }
  errors_[frame_index(operation)][error_index(error)].fetch_add(1, std::memory_order_relaxed);
  if (operation == FrameType::HeartbeatRequest) {
    group_heartbeat_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  if (operation == FrameType::GroupOffsetCommitRequest ||
      operation == FrameType::OffsetCommitRequest) {
    group_commit_failures_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MetricsRegistry::record_rejection(protocol::FrameType operation, RejectionReason reason) {
  if (!enabled_) {
    return;
  }
  rejections_[frame_index(operation)][static_cast<std::size_t>(reason)].fetch_add(
      1, std::memory_order_relaxed);
}

void MetricsRegistry::record_connection_accepted() {
  if (enabled_) {
    connections_accepted_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MetricsRegistry::record_connection_rejected() {
  if (enabled_) {
    connections_rejected_limit_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MetricsRegistry::record_records_produced(std::uint64_t count) {
  if (enabled_) {
    records_produced_.fetch_add(count, std::memory_order_relaxed);
  }
}

void MetricsRegistry::record_records_fetched(std::uint64_t count) {
  if (enabled_) {
    records_fetched_.fetch_add(count, std::memory_order_relaxed);
  }
}

void MetricsRegistry::record_append_batch(std::uint64_t records) {
  if (!enabled_) {
    return;
  }
  append_batches_.fetch_add(1, std::memory_order_relaxed);
  append_batch_records_.observe(records);
}

void MetricsRegistry::record_group_rebalance() {
  if (enabled_) {
    group_rebalances_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MetricsRegistry::record_retention_run(std::uint64_t segments_deleted,
                                           std::uint64_t bytes_deleted) {
  if (!enabled_) {
    return;
  }
  retention_runs_.fetch_add(1, std::memory_order_relaxed);
  retention_deleted_segments_.fetch_add(segments_deleted, std::memory_order_relaxed);
  retention_deleted_bytes_.fetch_add(bytes_deleted, std::memory_order_relaxed);
}

void MetricsRegistry::record_retention_failure() {
  if (enabled_) {
    retention_failures_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MetricsRegistry::set_recovery(double duration_seconds, std::uint64_t records,
                                   std::uint64_t truncated_bytes) {
  if (!enabled_) {
    return;
  }
  std::lock_guard lock{recovery_mutex_};
  recovery_duration_seconds_ = duration_seconds;
  recovered_records_ = records;
  truncated_bytes_ = truncated_bytes;
}

void MetricsRegistry::record_metrics_scrape() {
  if (enabled_) {
    metrics_scrapes_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MetricsRegistry::record_metrics_render_duration(double duration_seconds) {
  if (enabled_) {
    metrics_render_duration_.observe(duration_seconds);
  }
}

void MetricsRegistry::record_metrics_render_failure() {
  if (enabled_) {
    metrics_render_failures_.fetch_add(1, std::memory_order_relaxed);
  }
}

RegistrySnapshot MetricsRegistry::snapshot() const {
  RegistrySnapshot result;
  for (std::size_t frame = 0; frame < kMetricFrameTypeCount; ++frame) {
    result.requests[frame] = requests_[frame].load(std::memory_order_relaxed);
    result.request_durations[frame] = request_durations_[frame].snapshot();
    for (std::size_t error = 0; error < kMetricErrorCodeCount; ++error) {
      result.errors[frame][error] = errors_[frame][error].load(std::memory_order_relaxed);
    }
    for (std::size_t reason = 0; reason < static_cast<std::size_t>(RejectionReason::Count);
         ++reason) {
      result.rejections[frame][reason] = rejections_[frame][reason].load(std::memory_order_relaxed);
    }
  }
  result.connections_accepted = connections_accepted_.load(std::memory_order_relaxed);
  result.connections_rejected_limit = connections_rejected_limit_.load(std::memory_order_relaxed);
  result.protocol_received_bytes = protocol_received_bytes_.load(std::memory_order_relaxed);
  result.protocol_sent_bytes = protocol_sent_bytes_.load(std::memory_order_relaxed);
  result.records_produced = records_produced_.load(std::memory_order_relaxed);
  result.records_fetched = records_fetched_.load(std::memory_order_relaxed);
  result.append_batches = append_batches_.load(std::memory_order_relaxed);
  result.append_batch_records = append_batch_records_.snapshot();
  result.group_rebalances = group_rebalances_.load(std::memory_order_relaxed);
  result.group_heartbeat_failures = group_heartbeat_failures_.load(std::memory_order_relaxed);
  result.group_commit_failures = group_commit_failures_.load(std::memory_order_relaxed);
  result.retention_runs = retention_runs_.load(std::memory_order_relaxed);
  result.retention_failures = retention_failures_.load(std::memory_order_relaxed);
  result.retention_deleted_segments = retention_deleted_segments_.load(std::memory_order_relaxed);
  result.retention_deleted_bytes = retention_deleted_bytes_.load(std::memory_order_relaxed);
  result.metrics_scrapes = metrics_scrapes_.load(std::memory_order_relaxed);
  result.metrics_render_duration = metrics_render_duration_.snapshot();
  result.metrics_render_failures = metrics_render_failures_.load(std::memory_order_relaxed);
  {
    std::lock_guard lock{recovery_mutex_};
    result.recovery_duration_seconds = recovery_duration_seconds_;
    result.recovered_records = recovered_records_;
    result.truncated_bytes = truncated_bytes_;
  }
  return result;
}

std::string prometheus_label_escape(std::string_view value) {
  std::ostringstream out;
  for (const char ch : value) {
    if (ch == '\\') {
      out << "\\\\";
    } else if (ch == '"') {
      out << "\\\"";
    } else if (ch == '\n') {
      out << "\\n";
    } else {
      out << ch;
    }
  }
  return out.str();
}

std::string_view rejection_reason_name(RejectionReason reason) {
  switch (reason) {
  case RejectionReason::AppendQueue:
    return "append_queue";
  case RejectionReason::LongPollLimit:
    return "long_poll_limit";
  case RejectionReason::ConnectionLimit:
    return "connection_limit";
  case RejectionReason::Count:
    break;
  }
  return "unknown";
}

std::string render_prometheus(const RuntimeMetricsSnapshot& snapshot) {
  std::ostringstream out;
  const auto& registry = snapshot.registry;

  family(out, "boltstream_build_info", "BoltStream build and protocol metadata.", "gauge");
  out << "boltstream_build_info"
      << labels({{"version", snapshot.build_info.version},
                 {"git_sha", snapshot.build_info.git_sha},
                 {"build_type", snapshot.build_info.build_type},
                 {"compiler", snapshot.build_info.compiler},
                 {"protocol_version", snapshot.build_info.protocol_version},
                 {"storage_format_version", snapshot.build_info.storage_format_version}})
      << " 1\n";
  family(out, "boltstream_uptime_seconds", "Seconds since BoltStream started.", "gauge");
  sample(out, "boltstream_uptime_seconds", snapshot.uptime_seconds);
  family(out, "boltstream_ready", "Whether BoltStream is ready to serve broker traffic.", "gauge");
  sample(out, "boltstream_ready", static_cast<std::uint64_t>(snapshot.ready ? 1U : 0U));

  family(out, "boltstream_connections_active", "Current broker protocol connections.", "gauge");
  sample(out, "boltstream_connections_active", snapshot.connections_active);
  family(out, "boltstream_connections_accepted_total",
         "Broker protocol connections accepted since startup.", "counter");
  sample(out, "boltstream_connections_accepted_total", registry.connections_accepted);
  family(out, "boltstream_connections_rejected_total",
         "Broker protocol connections rejected since startup.", "counter");
  out << "boltstream_connections_rejected_total" << labels({{"reason", "limit"}}) << ' '
      << registry.connections_rejected_limit << '\n';

  family(out, "boltstream_requests_total", "Decoded broker requests since startup.", "counter");
  for (const auto operation : kRequestOperations) {
    out << "boltstream_requests_total" << labels({{"operation", operation_name(operation)}}) << ' '
        << registry.requests[static_cast<std::size_t>(operation)] << '\n';
  }
  family(out, "boltstream_request_errors_total", "Broker request errors since startup.", "counter");
  for (const auto operation : kRequestOperations) {
    for (std::size_t error = 1; error < kMetricErrorCodeCount; ++error) {
      const auto count = registry.errors[static_cast<std::size_t>(operation)][error];
      if (count == 0) {
        continue;
      }
      out << "boltstream_request_errors_total"
          << labels({{"operation", operation_name(operation)},
                     {"error_code", std::string{protocol::error_code_name(
                                        static_cast<protocol::ErrorCode>(error))}}})
          << ' ' << count << '\n';
    }
  }
  family(out, "boltstream_request_duration_seconds", "Broker request duration in seconds.",
         "histogram");
  for (const auto operation : kRequestOperations) {
    render_histogram(out, "boltstream_request_duration_seconds", operation_name(operation),
                     registry.request_durations[static_cast<std::size_t>(operation)]);
  }
  family(out, "boltstream_protocol_received_bytes_total",
         "Broker protocol frame bytes received since startup.", "counter");
  sample(out, "boltstream_protocol_received_bytes_total", registry.protocol_received_bytes);
  family(out, "boltstream_protocol_sent_bytes_total",
         "Broker protocol frame bytes sent since startup.", "counter");
  sample(out, "boltstream_protocol_sent_bytes_total", registry.protocol_sent_bytes);
  family(out, "boltstream_records_produced_total", "Records appended since startup.", "counter");
  sample(out, "boltstream_records_produced_total", registry.records_produced);
  family(out, "boltstream_records_fetched_total", "Records fetched since startup.", "counter");
  sample(out, "boltstream_records_fetched_total", registry.records_fetched);
  family(out, "boltstream_append_batches_total", "Storage append batches flushed since startup.",
         "counter");
  sample(out, "boltstream_append_batches_total", registry.append_batches);
  family(out, "boltstream_append_batch_records", "Records contained in storage append batches.",
         "histogram");
  {
    std::uint64_t cumulative = 0;
    for (std::size_t index = 0; index < kAppendBatchRecordBuckets.size(); ++index) {
      cumulative += registry.append_batch_records.buckets[index];
      out << "boltstream_append_batch_records_bucket"
          << labels({{"le", std::to_string(kAppendBatchRecordBuckets[index])}}) << ' ' << cumulative
          << '\n';
    }
    out << "boltstream_append_batch_records_bucket" << labels({{"le", "+Inf"}}) << ' '
        << registry.append_batch_records.count << '\n';
    out << "boltstream_append_batch_records_sum " << registry.append_batch_records.sum << '\n';
    out << "boltstream_append_batch_records_count " << registry.append_batch_records.count << '\n';
  }
  family(out, "boltstream_runtime_io_workers", "Configured broker I/O event-loop workers.",
         "gauge");
  sample(out, "boltstream_runtime_io_workers", snapshot.io_workers);
  family(out, "boltstream_runtime_append_workers", "Configured storage append workers.", "gauge");
  sample(out, "boltstream_runtime_append_workers", snapshot.append_workers);

  family(out, "boltstream_partition_append_queue_depth",
         "Current active and pending append work by partition.", "gauge");
  family(out, "boltstream_partition_append_queue_capacity",
         "Configured append queue capacity by partition.", "gauge");
  family(out, "boltstream_partition_segments", "Current log segments by partition.", "gauge");
  family(out, "boltstream_partition_log_bytes", "Current retained log bytes by partition.",
         "gauge");
  family(out, "boltstream_partition_earliest_offset", "Current partition low watermark.", "gauge");
  family(out, "boltstream_partition_next_offset", "Current partition high watermark.", "gauge");
  family(out, "boltstream_retention_retained_bytes", "Current retained bytes by partition.",
         "gauge");
  for (const auto& partition : snapshot.partitions) {
    const auto resource_labels =
        labels({{"topic", partition.topic}, {"partition", std::to_string(partition.partition)}});
    out << "boltstream_partition_append_queue_depth" << resource_labels << ' '
        << partition.append_queue_depth << '\n';
    out << "boltstream_partition_append_queue_capacity" << resource_labels << ' '
        << partition.append_queue_capacity << '\n';
    out << "boltstream_partition_segments" << resource_labels << ' ' << partition.segments << '\n';
    out << "boltstream_partition_log_bytes" << resource_labels << ' ' << partition.log_bytes
        << '\n';
    out << "boltstream_partition_earliest_offset" << resource_labels << ' '
        << partition.earliest_offset << '\n';
    out << "boltstream_partition_next_offset" << resource_labels << ' ' << partition.next_offset
        << '\n';
    out << "boltstream_retention_retained_bytes" << resource_labels << ' ' << partition.log_bytes
        << '\n';
  }
  family(out, "boltstream_long_poll_waiters", "Current long-poll fetch waiters.", "gauge");
  sample(out, "boltstream_long_poll_waiters", snapshot.long_poll_waiters);
  family(out, "boltstream_rejected_requests_total", "Requests rejected by bounded limits.",
         "counter");
  for (const auto operation : kRequestOperations) {
    for (std::size_t reason = 0; reason < static_cast<std::size_t>(RejectionReason::Count);
         ++reason) {
      const auto count = registry.rejections[static_cast<std::size_t>(operation)][reason];
      if (count == 0) {
        continue;
      }
      out << "boltstream_rejected_requests_total"
          << labels({{"operation", operation_name(operation)},
                     {"reason",
                      std::string{rejection_reason_name(static_cast<RejectionReason>(reason))}}})
          << ' ' << count << '\n';
    }
  }

  family(out, "boltstream_topics", "Current topics.", "gauge");
  sample(out, "boltstream_topics", snapshot.topic_count);
  family(out, "boltstream_partitions", "Current partitions.", "gauge");
  sample(out, "boltstream_partitions", snapshot.partition_count);
  family(out, "boltstream_storage_capacity_bytes", "Data filesystem capacity in bytes.", "gauge");
  sample(out, "boltstream_storage_capacity_bytes", snapshot.storage_capacity_bytes);
  family(out, "boltstream_storage_available_bytes", "Data filesystem available bytes.", "gauge");
  sample(out, "boltstream_storage_available_bytes", snapshot.storage_available_bytes);
  family(out, "boltstream_storage_recovery_duration_seconds",
         "Duration of startup storage recovery in seconds.", "gauge");
  sample(out, "boltstream_storage_recovery_duration_seconds", registry.recovery_duration_seconds);
  family(out, "boltstream_storage_recovered_records", "Records scanned during startup recovery.",
         "gauge");
  sample(out, "boltstream_storage_recovered_records", registry.recovered_records);
  family(out, "boltstream_storage_truncated_bytes", "Tail bytes truncated during recovery.",
         "gauge");
  sample(out, "boltstream_storage_truncated_bytes", registry.truncated_bytes);

  family(out, "boltstream_consumer_group_members", "Current coordinated group members.", "gauge");
  family(out, "boltstream_consumer_group_generation", "Current coordinated group generation.",
         "gauge");
  for (const auto& group : snapshot.groups) {
    const auto group_labels = labels({{"group", group.group}, {"topic", group.topic}});
    out << "boltstream_consumer_group_members" << group_labels << ' ' << group.members << '\n';
    out << "boltstream_consumer_group_generation" << group_labels << ' ' << group.generation
        << '\n';
  }
  family(out, "boltstream_consumer_group_lag_records", "Durable consumer group lag in records.",
         "gauge");
  family(out, "boltstream_consumer_group_offset_out_of_range",
         "Whether a durable group offset is outside retained partition bounds.", "gauge");
  for (const auto& lag : snapshot.lags) {
    const auto lag_labels = labels(
        {{"group", lag.group}, {"topic", lag.topic}, {"partition", std::to_string(lag.partition)}});
    out << "boltstream_consumer_group_lag_records" << lag_labels << ' ' << lag.lag << '\n';
    out << "boltstream_consumer_group_offset_out_of_range" << lag_labels << ' '
        << (lag.offset_out_of_range ? 1 : 0) << '\n';
  }
  family(out, "boltstream_consumer_group_rebalances_total", "Group rebalances since startup.",
         "counter");
  sample(out, "boltstream_consumer_group_rebalances_total", registry.group_rebalances);
  family(out, "boltstream_consumer_group_heartbeat_failures_total",
         "Group heartbeat failures since startup.", "counter");
  sample(out, "boltstream_consumer_group_heartbeat_failures_total",
         registry.group_heartbeat_failures);
  family(out, "boltstream_consumer_group_commit_failures_total",
         "Group commit failures since startup.", "counter");
  sample(out, "boltstream_consumer_group_commit_failures_total", registry.group_commit_failures);

  family(out, "boltstream_retention_runs_total", "Retention passes since startup.", "counter");
  sample(out, "boltstream_retention_runs_total", registry.retention_runs);
  family(out, "boltstream_retention_failures_total", "Retention failures since startup.",
         "counter");
  sample(out, "boltstream_retention_failures_total", registry.retention_failures);
  family(out, "boltstream_retention_deleted_segments_total",
         "Segments deleted by retention since startup.", "counter");
  sample(out, "boltstream_retention_deleted_segments_total", registry.retention_deleted_segments);
  family(out, "boltstream_retention_deleted_bytes_total",
         "Bytes deleted by retention since startup.", "counter");
  sample(out, "boltstream_retention_deleted_bytes_total", registry.retention_deleted_bytes);

  family(out, "boltstream_metrics_scrapes_total", "Metrics scrapes served since startup.",
         "counter");
  sample(out, "boltstream_metrics_scrapes_total", registry.metrics_scrapes);
  family(out, "boltstream_metrics_render_duration_seconds", "Metrics render duration in seconds.",
         "histogram");
  render_unlabeled_histogram(out, "boltstream_metrics_render_duration_seconds",
                             registry.metrics_render_duration);
  family(out, "boltstream_metrics_render_failures_total", "Metrics render failures.", "counter");
  sample(out, "boltstream_metrics_render_failures_total", registry.metrics_render_failures);
  return out.str();
}

} // namespace boltstream::observability
