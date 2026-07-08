#include <iostream>
#include <string_view>

namespace {

void usage() {
  std::cout << "Usage: boltstream-bench --dry-run\n"
               "\n"
               "Phase 5 keeps the benchmark command shell only. Real throughput and latency "
               "benchmarks start in the benchmark phase.\n";
}

} // namespace

int main(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg{argv[index]};
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--dry-run") {
      std::cout << "{\"service\":\"boltstream-bench\",\"status\":\"dry_run\","
                   "\"published_numbers\":false}\n";
      return 0;
    }
  }

  std::cerr << "boltstream-bench: real benchmark mode starts in the benchmark phase\n";
  usage();
  return 2;
}
