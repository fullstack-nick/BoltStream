#include <iostream>
#include <string_view>

namespace {

void usage() {
  std::cout << "Usage: boltstream-consumer --topic TOPIC [--from beginning|latest|OFFSET] "
               "[--group GROUP]\n"
               "\n"
               "Phase 1 provides the CLI shell only. Fetch protocol support starts in Phase 2.\n";
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

  std::cerr << "boltstream-consumer: protocol starts in Phase 2\n";
  usage();
  return 2;
}
