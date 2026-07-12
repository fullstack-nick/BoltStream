#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace boltstream::recovery {

enum class FaultScenario { TornRecord, PartialBatch, StaleIndex };

struct ProofResult {
  FaultScenario scenario{FaultScenario::TornRecord};
  std::size_t records_recovered{0};
  std::uint64_t next_offset{0};
  std::uintmax_t bytes_truncated{0};
  std::size_t indexes_rebuilt{0};
  std::uintmax_t log_bytes_before{0};
  std::uintmax_t log_bytes_after{0};
  std::uintmax_t index_bytes_after{0};
};

std::string_view scenario_name(FaultScenario scenario);
FaultScenario parse_scenario(std::string_view name);
std::vector<FaultScenario> scenarios();

// Creates three committed records and leaves a deterministic incomplete tail/index
// mutation on disk. Callers that model a process crash must terminate immediately
// after this function returns.
void stage_crash_state(const std::filesystem::path& root, FaultScenario scenario);

// Reopens and repairs a staged partition, throwing unless the logical log contains
// exactly the committed prefix and the physical recovery invariants hold.
ProofResult verify_crash_recovery(const std::filesystem::path& root, FaultScenario scenario);

} // namespace boltstream::recovery
