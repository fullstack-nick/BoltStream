#include "boltstream/recovery/proof.h"

#include "boltstream/compression/compression.h"
#include "boltstream/protocol/protocol.h"
#include "boltstream/storage/partition_log.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>

namespace boltstream::recovery {
namespace {

constexpr std::string_view kTopic{"phase13-recovery"};
constexpr std::array<std::string_view, 3> kKeys{"alpha", "beta", "gamma"};
constexpr std::array<std::string_view, 3> kValues{"committed-0", "committed-1", "committed-2"};
constexpr std::uintmax_t kIndexEntryBytes = 20;

std::vector<std::uint8_t> bytes(std::string_view value) { return {value.begin(), value.end()}; }

storage::PartitionLog open_log(const std::filesystem::path& root) {
  return storage::PartitionLog::open(
      {root, std::string{kTopic}, 0, storage::kDefaultMaxSegmentBytes});
}

void append_seed(storage::PartitionLog& log) {
  for (std::size_t index = 0; index < kKeys.size(); ++index) {
    const auto key = bytes(kKeys[index]);
    const auto value = bytes(kValues[index]);
    (void)log.append(key, value);
  }
}

std::filesystem::path only_file(const std::filesystem::path& directory,
                                std::string_view extension) {
  std::vector<std::filesystem::path> matches;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == extension) {
      matches.push_back(entry.path());
    }
  }
  if (matches.size() != 1) {
    throw std::runtime_error("expected exactly one " + std::string{extension} + " file in " +
                             directory.string());
  }
  return matches.front();
}

std::vector<std::uint8_t> donor_suffix(const std::filesystem::path& donor_root,
                                       FaultScenario scenario) {
  std::uintmax_t prefix_bytes = 0;
  std::filesystem::path log_path;
  {
    auto donor = open_log(donor_root);
    append_seed(donor);
    const auto summaries = donor.segment_summaries();
    if (summaries.size() != 1) {
      throw std::runtime_error("donor unexpectedly rolled segments");
    }
    prefix_bytes = summaries.front().log_bytes;
    log_path = summaries.front().log_path;

    if (scenario == FaultScenario::TornRecord) {
      const auto key = bytes("delta");
      const auto value = bytes("must-not-survive");
      (void)donor.append(key, value);
    } else {
      const std::array<protocol::BatchRecord, 2> records{
          protocol::BatchRecord{bytes("delta"), bytes("batch-must-not-survive-0")},
          protocol::BatchRecord{bytes("epsilon"), bytes("batch-must-not-survive-1")}};
      const auto canonical = protocol::encode_record_set(records);
      const auto encoded = compression::compress(compression::Codec::Zstd, canonical, 3);
      const std::array<storage::AppendRecord, 2> decoded{
          storage::AppendRecord{records[0].key, records[0].message},
          storage::AppendRecord{records[1].key, records[1].message}};
      (void)donor.append_encoded_batch(compression::Codec::Zstd, encoded,
                                       static_cast<std::uint32_t>(canonical.size()), decoded);
    }
  }

  std::ifstream in{log_path, std::ios::binary};
  if (!in) {
    throw std::runtime_error("failed to read donor segment");
  }
  in.seekg(0, std::ios::end);
  const auto size = static_cast<std::uintmax_t>(in.tellg());
  if (size <= prefix_bytes + 1) {
    throw std::runtime_error("donor mutation was not appended");
  }
  const auto suffix_size = static_cast<std::size_t>(size - prefix_bytes);
  std::vector<std::uint8_t> suffix(suffix_size);
  in.seekg(static_cast<std::streamoff>(prefix_bytes));
  in.read(reinterpret_cast<char*>(suffix.data()), static_cast<std::streamsize>(suffix.size()));
  if (!in) {
    throw std::runtime_error("failed to read donor suffix");
  }
  return suffix;
}

} // namespace

std::string_view scenario_name(FaultScenario scenario) {
  switch (scenario) {
  case FaultScenario::TornRecord:
    return "torn-record";
  case FaultScenario::PartialBatch:
    return "partial-batch";
  case FaultScenario::StaleIndex:
    return "stale-index";
  }
  throw std::invalid_argument("unknown crash scenario");
}

FaultScenario parse_scenario(std::string_view name) {
  for (const auto scenario : scenarios()) {
    if (scenario_name(scenario) == name) {
      return scenario;
    }
  }
  throw std::invalid_argument("unknown crash scenario: " + std::string{name});
}

std::vector<FaultScenario> scenarios() {
  return {FaultScenario::TornRecord, FaultScenario::PartialBatch, FaultScenario::StaleIndex};
}

void stage_crash_state(const std::filesystem::path& root, FaultScenario scenario) {
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    throw std::runtime_error("failed to create crash root: " + ec.message());
  }

  {
    auto log = open_log(root);
    append_seed(log);
  }

  const auto partition = storage::partition_directory(root, kTopic, 0);
  const auto log_path = only_file(partition, ".log");
  const auto index_path = only_file(partition, ".index");
  if (scenario == FaultScenario::StaleIndex) {
    const std::array<std::uint8_t, 7> partial_index{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    std::ofstream out{index_path, std::ios::binary | std::ios::app};
    out.write(reinterpret_cast<const char*>(partial_index.data()),
              static_cast<std::streamsize>(partial_index.size()));
    out.flush();
    if (!out) {
      throw std::runtime_error("failed to stage partial index append");
    }
    return;
  }

  const auto donor_root = root / "donor";
  const auto suffix = donor_suffix(donor_root, scenario);
  const auto partial_size = std::max<std::size_t>(4, suffix.size() / 2);
  std::ofstream out{log_path, std::ios::binary | std::ios::app};
  out.write(reinterpret_cast<const char*>(suffix.data()),
            static_cast<std::streamsize>(partial_size));
  out.flush();
  if (!out) {
    throw std::runtime_error("failed to stage incomplete log append");
  }
  out.close();
  std::filesystem::remove_all(donor_root, ec);
}

ProofResult verify_crash_recovery(const std::filesystem::path& root, FaultScenario scenario) {
  const auto partition = storage::partition_directory(root, kTopic, 0);
  const auto log_path = only_file(partition, ".log");
  ProofResult result;
  result.scenario = scenario;
  result.log_bytes_before = std::filesystem::file_size(log_path);

  auto recovered = open_log(root);
  const auto records = recovered.read_from(0, 100, 0);
  const auto& stats = recovered.recovery_stats();
  result.records_recovered = records.size();
  result.next_offset = recovered.next_offset();
  result.bytes_truncated = stats.bytes_truncated;
  result.indexes_rebuilt = stats.indexes_rebuilt;
  result.log_bytes_after = std::filesystem::file_size(log_path);
  result.index_bytes_after = std::filesystem::file_size(only_file(partition, ".index"));

  if (records.size() != kKeys.size() || result.next_offset != kKeys.size() ||
      result.indexes_rebuilt != 1 || result.index_bytes_after != kKeys.size() * kIndexEntryBytes) {
    throw std::runtime_error("recovered storage shape does not match the committed prefix");
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const std::string key{records[index].key.begin(), records[index].key.end()};
    const std::string value{records[index].value.begin(), records[index].value.end()};
    if (records[index].metadata.offset != index || key != kKeys[index] || value != kValues[index]) {
      throw std::runtime_error("recovered logical records do not match the committed prefix");
    }
  }
  if (scenario != FaultScenario::StaleIndex &&
      (result.bytes_truncated == 0 || result.log_bytes_after >= result.log_bytes_before)) {
    throw std::runtime_error("incomplete log suffix was not truncated");
  }
  if (scenario == FaultScenario::StaleIndex && result.bytes_truncated != 0) {
    throw std::runtime_error("index-only recovery unexpectedly truncated the log");
  }
  return result;
}

} // namespace boltstream::recovery
