#include <benchmark/benchmark.h>

namespace {

void placeholder_phase1_benchmark(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(state.iterations());
  }
}

BENCHMARK(placeholder_phase1_benchmark);

} // namespace

BENCHMARK_MAIN();
