#include "boltstream/replication/simulation.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string json_escape(std::string_view value) {
  std::string out;
  for (const auto ch : value) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  return out;
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path root = std::filesystem::temp_directory_path() / "boltstream-phase12";
  auto codec = boltstream::compression::Codec::Zstd;
  bool keep_data = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg{argv[index]};
    if (arg == "--root" && index + 1 < argc) {
      root = argv[++index];
    } else if (arg == "--compression" && index + 1 < argc) {
      const std::string_view value{argv[++index]};
      if (value == "none") {
        codec = boltstream::compression::Codec::None;
      } else if (value == "zstd") {
        codec = boltstream::compression::Codec::Zstd;
      } else {
        std::cerr << "boltstream-replication-sim: compression must be none or zstd\n";
        return 2;
      }
    } else if (arg == "--keep-data") {
      keep_data = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: boltstream-replication-sim [--root PATH] "
                   "[--compression none|zstd] [--keep-data]\n";
      return 0;
    } else {
      std::cerr << "boltstream-replication-sim: unknown or incomplete argument: " << arg << '\n';
      return 2;
    }
  }

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  try {
    boltstream::replication::SimulationOptions options;
    options.leader_data_dir = root / "leader";
    options.follower_data_dir = root / "follower";
    boltstream::replication::ReplicationSimulation simulation{options};
    const std::vector<std::uint8_t> key{'k'};
    const std::vector<std::uint8_t> value(128, static_cast<std::uint8_t>('r'));
    const std::vector<boltstream::storage::AppendRecord> records{{key, value}, {key, value}};

    simulation.set_follower_available(false);
    simulation.produce(records, codec, boltstream::replication::AcknowledgementMode::Leader);
    bool timeout_observed = false;
    try {
      simulation.produce(records, codec, boltstream::replication::AcknowledgementMode::All,
                         std::chrono::milliseconds{10});
    } catch (const boltstream::replication::ReplicationTimeout&) {
      timeout_observed = true;
    }
    simulation.set_follower_available(true);
    const auto fetches = simulation.catch_up();
    const auto before_restart = simulation.status().follower_next_offset;
    simulation.reopen_follower();
    const auto after_restart = simulation.status().follower_next_offset;
    simulation.produce(records, codec, boltstream::replication::AcknowledgementMode::All);
    const auto status = simulation.status();
    const auto leader = simulation.leader_records();
    const auto follower = simulation.follower_records();
    const auto exact = leader.size() == follower.size() &&
                       std::equal(leader.begin(), leader.end(), follower.begin(),
                                  [](const auto& lhs, const auto& rhs) {
                                    return lhs.metadata.offset == rhs.metadata.offset &&
                                           lhs.key == rhs.key && lhs.value == rhs.value;
                                  });

    std::cout << "{\"status\":\"ok\",\"topic\":\"" << json_escape(status.assignment.topic)
              << "\",\"leader\":\"" << json_escape(status.assignment.leader_id)
              << "\",\"follower\":\"" << json_escape(status.assignment.follower_ids.front())
              << "\",\"codec\":\"" << boltstream::compression::codec_name(codec)
              << "\",\"leader_next_offset\":" << status.leader_next_offset
              << ",\"follower_next_offset\":" << status.follower_next_offset
              << ",\"lag_records\":" << status.lag_records << ",\"replication_fetches\":" << fetches
              << ",\"leader_ack_while_offline\":true"
              << ",\"all_timeout_observed\":" << (timeout_observed ? "true" : "false")
              << ",\"restart_offset_before\":" << before_restart
              << ",\"restart_offset_after\":" << after_restart
              << ",\"records_exact\":" << (exact ? "true" : "false") << "}\n";
    std::cout << simulation.prometheus_metrics();
    if (!keep_data) {
      std::filesystem::remove_all(root, ignored);
    }
    return timeout_observed && before_restart == after_restart && status.lag_records == 0 && exact
               ? 0
               : 1;
  } catch (const std::exception& error) {
    std::cerr << "boltstream-replication-sim: " << error.what() << '\n';
    if (!keep_data) {
      std::filesystem::remove_all(root, ignored);
    }
    return 1;
  }
}
