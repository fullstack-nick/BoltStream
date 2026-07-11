# Native Windows Release Benchmarks

Secondary comparison for exact commit `14d225abe1d5` on Intel64 Family 6 Model 186 Stepping 2, GenuineIntel, 20 logical CPUs, 33960161280 bytes RAM, MSVC 19.44.35228.0. GCP `e2-micro` remains the headline environment.

These correctness-sized secondary runs use four partitions, 16 clients, 256-byte values, 16-byte keys, and three repetitions. Produce throughput measures three seconds after a two-second warmup; latency uses 200 warmup plus 2,000 measured messages; fetch uses an authenticated protocol preload of 5,000 records. These parameters intentionally differ from the GCP headline workloads and are retained in the JSON.

| Profile | Produce records/s | Produce CV | p50 (us) | p95 (us) | p99 (us) | Fetch records/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| single-threaded | 132 | 7.31% | 32161.300 | 139029.700 | 161401.600 | 4080 |
| worker-event-loops | 193 | 30.13% | 59242.300 | 135199.800 | 164726.100 | 8003 |
| batched-writes | 1010 | 43.58% | 11656.200 | 34733.800 | 41206.700 | 8467 |

Every repetition and its exact workload configuration is retained in [the Windows JSON](../benchmarks/results/phase-10-windows-release.json).
