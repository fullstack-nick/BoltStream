#include "boltstream/protocol/protocol.h"
#include "boltstream/storage/partition_log.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

class TempDirectory {
public:
  explicit TempDirectory(std::string_view name) {
    path_ = std::filesystem::temp_directory_path() /
            ("boltstream-microbench-" + std::string{name} + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::vector<std::uint8_t> filled(std::size_t size, std::uint8_t value) {
  return std::vector<std::uint8_t>(size, value);
}

void protocol_encode_decode(benchmark::State& state) {
  const auto key = filled(16, static_cast<std::uint8_t>('k'));
  const auto message =
      filled(static_cast<std::size_t>(state.range(0)), static_cast<std::uint8_t>('v'));
  const auto payload = boltstream::protocol::encode_produce_request("bench", key, message);
  for (auto _ : state) {
    (void)_;
    const auto encoded = boltstream::protocol::encode_frame(
        boltstream::protocol::FrameType::ProduceRequest, 42, payload);
    const auto decoded =
        boltstream::protocol::decode_frame(encoded, boltstream::protocol::kDefaultMaxFrameBytes);
    benchmark::DoNotOptimize(decoded.frame.payload.data());
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(payload.size()));
}

void storage_append_single(benchmark::State& state) {
  const auto key = filled(16, static_cast<std::uint8_t>('k'));
  const auto value = filled(256, static_cast<std::uint8_t>('v'));
  TempDirectory temp{"single"};
  auto log = boltstream::storage::PartitionLog::open({temp.path(), "bench", 0});
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(log.append(key, value));
  }
  state.SetItemsProcessed(state.iterations());
}

void storage_append_batch(benchmark::State& state) {
  const auto key = filled(16, static_cast<std::uint8_t>('k'));
  const auto value = filled(256, static_cast<std::uint8_t>('v'));
  std::vector<boltstream::storage::AppendRecord> records(static_cast<std::size_t>(state.range(0)),
                                                         {key, value});
  TempDirectory temp{"batch"};
  auto log = boltstream::storage::PartitionLog::open({temp.path(), "bench", 0});
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(log.append_batch(records));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

void storage_segment_roll(benchmark::State& state) {
  const auto key = filled(16, static_cast<std::uint8_t>('k'));
  const auto value = filled(256, static_cast<std::uint8_t>('v'));
  TempDirectory temp{"roll"};
  auto log = boltstream::storage::PartitionLog::open({temp.path(), "bench", 0, 512});
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(log.append(key, value));
  }
  state.SetItemsProcessed(state.iterations());
}

void storage_read(benchmark::State& state) {
  const auto key = filled(16, static_cast<std::uint8_t>('k'));
  const auto value = filled(256, static_cast<std::uint8_t>('v'));
  TempDirectory temp{"read"};
  auto log = boltstream::storage::PartitionLog::open({temp.path(), "bench", 0});
  for (std::uint64_t index = 0; index < 1000; ++index) {
    (void)log.append(key, value);
  }
  for (auto _ : state) {
    (void)_;
    const auto records = log.read_from(0, 1000, 0);
    benchmark::DoNotOptimize(records.data());
  }
  state.SetItemsProcessed(state.iterations() * 1000);
}

BENCHMARK(protocol_encode_decode)->Arg(64)->Arg(256)->Arg(1024);
BENCHMARK(storage_append_single);
BENCHMARK(storage_append_batch)->Arg(8)->Arg(32);
BENCHMARK(storage_segment_roll);
BENCHMARK(storage_read);

} // namespace

BENCHMARK_MAIN();
