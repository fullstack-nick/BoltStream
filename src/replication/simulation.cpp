#include "boltstream/replication/simulation.h"

#include "boltstream/protocol/protocol.h"

#include <algorithm>
#include <sstream>
#include <thread>

namespace boltstream::replication {
namespace {

std::string escape_label(std::string_view value) {
  std::string escaped;
  for (const auto ch : value) {
    if (ch == '\\' || ch == '"' || ch == '\n') {
      escaped.push_back('\\');
      escaped.push_back(ch == '\n' ? 'n' : ch);
    } else {
      escaped.push_back(ch);
    }
  }
  return escaped;
}

} // namespace

ReplicationSimulation::ReplicationSimulation(SimulationOptions options)
    : options_(std::move(options)), leader_(open_log(options_.leader_data_dir)),
      follower_(open_log(options_.follower_data_dir)) {
  if (options_.leader_data_dir.empty() || options_.follower_data_dir.empty()) {
    throw std::invalid_argument("leader and follower data directories are required");
  }
  if (options_.leader_data_dir == options_.follower_data_dir) {
    throw std::invalid_argument("leader and follower data directories must differ");
  }
  if (options_.leader_id.empty() || options_.follower_id.empty() ||
      options_.leader_id == options_.follower_id) {
    throw std::invalid_argument("leader and follower broker ids must be distinct");
  }
  if (follower_.next_offset() > leader_.next_offset()) {
    throw std::runtime_error("follower offset is ahead of leader");
  }
}

storage::PartitionLog ReplicationSimulation::open_log(const std::filesystem::path& data_dir) const {
  return storage::PartitionLog::open({data_dir, options_.topic, options_.partition,
                                      options_.max_segment_bytes, 0,
                                      options_.max_uncompressed_batch_bytes});
}

storage::BatchMetadata
ReplicationSimulation::produce(std::span<const storage::AppendRecord> records,
                               compression::Codec codec, AcknowledgementMode acknowledgement,
                               std::chrono::milliseconds timeout) {
  std::vector<protocol::BatchRecord> batch;
  batch.reserve(records.size());
  for (const auto& record : records) {
    batch.push_back(
        {{record.key.begin(), record.key.end()}, {record.value.begin(), record.value.end()}});
  }
  const auto canonical = protocol::encode_record_set(batch);
  const auto encoded = compression::compress(codec, canonical, 3);
  auto metadata = leader_.append_encoded_batch(
      codec, encoded, static_cast<std::uint32_t>(canonical.size()), records);

  if (acknowledgement == AcknowledgementMode::Leader) {
    return metadata;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (follower_.next_offset() < metadata.next_offset &&
         std::chrono::steady_clock::now() < deadline) {
    if (!replicate_once()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
  if (follower_.next_offset() < metadata.next_offset) {
    throw ReplicationTimeout{"replication=all timed out waiting for follower " +
                             options_.follower_id};
  }
  return metadata;
}

bool ReplicationSimulation::replicate_once() {
  if (!follower_available_) {
    return false;
  }
  const auto next = follower_.next_offset();
  if (next > leader_.next_offset()) {
    throw std::runtime_error("follower offset is ahead of leader");
  }
  if (next == leader_.next_offset()) {
    return false;
  }

  if (const auto encoded = leader_.read_encoded_batch(next)) {
    const auto records = leader_.read_from(next, encoded->metadata.record_count, 0);
    std::vector<storage::AppendRecord> append_records;
    append_records.reserve(records.size());
    for (const auto& record : records) {
      append_records.push_back({record.key, record.value});
    }
    const auto copied =
        follower_.append_encoded_batch(encoded->metadata.codec, encoded->encoded_records,
                                       encoded->metadata.logical_bytes, append_records);
    if (copied.base_offset != encoded->metadata.base_offset ||
        copied.next_offset != encoded->metadata.next_offset) {
      throw std::runtime_error("replicated batch offset mismatch");
    }
    return true;
  }

  const auto records = leader_.read_from(next, 1, 0);
  if (records.empty() || records.front().metadata.offset != next) {
    throw std::runtime_error("leader cannot supply follower offset");
  }
  const storage::AppendRecord append{records.front().key, records.front().value};
  const auto copied = follower_.append_batch(std::span<const storage::AppendRecord>{&append, 1});
  if (copied.front().offset != next) {
    throw std::runtime_error("replicated record offset mismatch");
  }
  return true;
}

std::size_t ReplicationSimulation::catch_up() {
  std::size_t fetches = 0;
  while (replicate_once()) {
    ++fetches;
  }
  return fetches;
}

void ReplicationSimulation::reopen_follower() { follower_ = open_log(options_.follower_data_dir); }

ReplicationStatus ReplicationSimulation::status() const {
  const auto leader_offset = leader_.next_offset();
  const auto follower_offset = follower_.next_offset();
  return {{options_.topic, options_.partition, options_.leader_id, {options_.follower_id}},
          leader_offset,
          follower_offset,
          leader_offset >= follower_offset ? leader_offset - follower_offset : 0,
          follower_available_};
}

std::string ReplicationSimulation::prometheus_metrics() const {
  const auto current = status();
  const auto base = "{topic=\"" + escape_label(options_.topic) + "\",partition=\"" +
                    std::to_string(options_.partition) + "\",leader=\"" +
                    escape_label(options_.leader_id) + "\",follower=\"" +
                    escape_label(options_.follower_id) + "\"}";
  std::ostringstream out;
  out << "# TYPE boltstream_replication_leader_next_offset gauge\n"
      << "boltstream_replication_leader_next_offset" << base << ' ' << current.leader_next_offset
      << '\n'
      << "# TYPE boltstream_replication_follower_next_offset gauge\n"
      << "boltstream_replication_follower_next_offset" << base << ' '
      << current.follower_next_offset << '\n'
      << "# TYPE boltstream_replication_lag_records gauge\n"
      << "boltstream_replication_lag_records" << base << ' ' << current.lag_records << '\n'
      << "# TYPE boltstream_replication_follower_available gauge\n"
      << "boltstream_replication_follower_available" << base << ' '
      << (current.follower_available ? 1 : 0) << '\n';
  return out.str();
}

std::vector<storage::Record> ReplicationSimulation::leader_records() const {
  return leader_.read_from(leader_.earliest_offset(),
                           static_cast<std::size_t>(leader_.next_offset()), 0);
}

std::vector<storage::Record> ReplicationSimulation::follower_records() const {
  return follower_.read_from(follower_.earliest_offset(),
                             static_cast<std::size_t>(follower_.next_offset()), 0);
}

std::string_view acknowledgement_mode_name(AcknowledgementMode mode) {
  return mode == AcknowledgementMode::Leader ? "leader" : "all";
}

} // namespace boltstream::replication
