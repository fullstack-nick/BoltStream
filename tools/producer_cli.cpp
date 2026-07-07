#include <iostream>
#include <string_view>

namespace {

void usage() {
  std::cout << "Usage: boltstream-producer --topic TOPIC --message VALUE [--key KEY]\n"
               "\n"
               "Phase 1 provides the CLI shell only. Produce protocol support starts in Phase 2.\n";
}

} // namespace

int main(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg{argv[index]};
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
  }

  std::cerr << "boltstream-producer: protocol starts in Phase 2\n";
  usage();
  return 2;
}
