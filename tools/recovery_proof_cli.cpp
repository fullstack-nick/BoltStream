#include "boltstream/recovery/proof.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <process.h>
#endif

namespace {

#ifndef _WIN32
std::string quote(const std::filesystem::path& path) {
  const auto value = path.string();
  if (value.find('"') != std::string::npos) {
    throw std::invalid_argument("paths containing quotes are unsupported");
  }
  return '"' + value + '"';
}
#endif

void print_result(const boltstream::recovery::ProofResult& result) {
  std::cout << "{\"scenario\":\"" << boltstream::recovery::scenario_name(result.scenario)
            << "\",\"worker_crashed\":true,\"records_recovered\":" << result.records_recovered
            << ",\"next_offset\":" << result.next_offset
            << ",\"bytes_truncated\":" << result.bytes_truncated
            << ",\"indexes_rebuilt\":" << result.indexes_rebuilt
            << ",\"log_bytes_before\":" << result.log_bytes_before
            << ",\"log_bytes_after\":" << result.log_bytes_after
            << ",\"index_bytes_after\":" << result.index_bytes_after << "}\n";
}

int launch_worker(const std::filesystem::path& executable, const std::filesystem::path& root,
                  boltstream::recovery::FaultScenario scenario) {
#ifdef _WIN32
  const auto executable_text = executable.string();
  const auto root_text = root.string();
  const auto scenario_text = std::string{boltstream::recovery::scenario_name(scenario)};
  const char* arguments[]{executable_text.c_str(), "--worker", scenario_text.c_str(), "--root",
                          root_text.c_str(),       nullptr};
  return static_cast<int>(_spawnv(_P_WAIT, executable_text.c_str(), arguments));
#else
  const auto command = quote(executable) + " --worker " +
                       std::string{boltstream::recovery::scenario_name(scenario)} + " --root " +
                       quote(root);
  return std::system(command.c_str());
#endif
}

} // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path root{"phase13-recovery-proof"};
    bool keep_data = false;
    bool worker = false;
    boltstream::recovery::FaultScenario worker_scenario{};
    for (int index = 1; index < argc; ++index) {
      const std::string argument{argv[index]};
      if (argument == "--root" && index + 1 < argc) {
        root = argv[++index];
      } else if (argument == "--worker" && index + 1 < argc) {
        worker = true;
        worker_scenario = boltstream::recovery::parse_scenario(argv[++index]);
      } else if (argument == "--keep-data") {
        keep_data = true;
      } else {
        throw std::invalid_argument("usage: boltstream-recovery-proof [--root PATH] "
                                    "[--keep-data] [--worker SCENARIO]");
      }
    }

    if (worker) {
      boltstream::recovery::stage_crash_state(root, worker_scenario);
      std::_Exit(86);
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto executable = std::filesystem::absolute(argv[0]);
    for (const auto scenario : boltstream::recovery::scenarios()) {
      const auto scenario_root = root / boltstream::recovery::scenario_name(scenario);
      const auto worker_status = launch_worker(executable, scenario_root, scenario);
      if (worker_status == 0) {
        throw std::runtime_error("crash worker exited normally");
      }
      print_result(boltstream::recovery::verify_crash_recovery(scenario_root, scenario));
    }
    std::cout << "{\"status\":\"ok\",\"scenarios\":3,\"committed_records\":3,"
                 "\"records_exact\":true,\"protocol_version\":5,\"storage_format_version\":3}\n";
    if (!keep_data) {
      std::filesystem::remove_all(root, ignored);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "recovery proof failed: " << error.what() << '\n';
    return 1;
  }
}
