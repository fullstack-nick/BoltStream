#pragma once

#include "boltstream/compression/compression.h"
#include "boltstream/storage/partition_log.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace boltstream::replication {

enum class AcknowledgementMode { Leader, All };

struct PartitionAssignment {
  std::string topic;
  std::uint16_t partition{0};
  std::string leader_id;
  std::vector<std::string> follower_ids;
};

struct SimulationOptions {
  std::filesystem::path leader_data_dir;
  std::filesystem::path follower_data_dir;
  std::string topic{"phase12"};
  std::uint16_t partition{0};
  std::string leader_id{"leader-1"};
  std::string follower_id{"follower-1"};
  std::uintmax_t max_segment_bytes{storage::kDefaultMaxSegmentBytes};
  std::uint32_t max_uncompressed_batch_bytes{1024U * 1024U};
};

struct ReplicationStatus {
  PartitionAssignment assignment;
  std::uint64_t leader_next_offset{0};
  std::uint64_t follower_next_offset{0};
  std::uint64_t lag_records{0};
  bool follower_available{true};
};

class ReplicationTimeout : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class ReplicationSimulation {
public:
  explicit ReplicationSimulation(SimulationOptions options);

  storage::BatchMetadata produce(std::span<const storage::AppendRecord> records,
                                 compression::Codec codec, AcknowledgementMode acknowledgement,
                                 std::chrono::milliseconds timeout = std::chrono::milliseconds{
                                     1000});
  bool replicate_once();
  std::size_t catch_up();
  void reopen_follower();
  void set_follower_available(bool available) { follower_available_ = available; }

  [[nodiscard]] ReplicationStatus status() const;
  [[nodiscard]] std::string prometheus_metrics() const;
  [[nodiscard]] std::vector<storage::Record> leader_records() const;
  [[nodiscard]] std::vector<storage::Record> follower_records() const;

private:
  [[nodiscard]] storage::PartitionLog open_log(const std::filesystem::path& data_dir) const;

  SimulationOptions options_;
  storage::PartitionLog leader_;
  storage::PartitionLog follower_;
  bool follower_available_{true};
};

std::string_view acknowledgement_mode_name(AcknowledgementMode mode);

} // namespace boltstream::replication
