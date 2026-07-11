# BoltStream Benchmarks

Canonical commit: `14d225abe1d5`

Environment: GCP `e2-micro`; Linux 6.17.0-1020-gcp; AMD EPYC 7B12; 2 logical CPUs; 1001922560 bytes RAM; GNU 13.3.0; Release build.

The client and broker run together on the VM over loopback. The e2-micro is shared-core and may burst, so these numbers describe this exact environment rather than dedicated-host capacity. Every profile uses protocol v4, storage format v2, 256-byte payloads, 16-byte keys, four partitions, metrics enabled, warning-level logs, and flush durability.

**Limited campaign:** the operator stopped the planned five-round run after two complete rotated rounds for every profile and a third complete round for single-threaded and batched-writes. Every completed profile triplet is retained. The interrupted worker-event-loops round three produced no complete local triplet and is excluded. These results are sufficient for bounded Phase 10 engineering evidence but are not a five-round capacity study.

The measured recommendation is **batched-writes**. It is an explicit benchmark profile; ordinary compiled, Compose, and GCP defaults remain compatibility-oriented.

## Headline Produce Results

| Profile | Median records/s | Min | Max | CV | Median MiB/s | p50 (us) | p95 (us) | p99 (us) | max (us) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| single-threaded | 104 | 71 | 125 | 27.13% | 0.031 | 258644.424 | 741374.534 | 753321.145 | 1992654.750 |
| worker-event-loops | 92 | 67 | 117 | 38.39% | 0.028 | 391006.978 | 746455.544 | 756699.619 | 1492236.219 |
| batched-writes | 180 | 171 | 191 | 5.46% | 0.054 | 12962.789 | 239372.586 | 243553.735 | 970660.144 |

## Fetch Results

| Profile | Median records/s | Min | Max | CV | Median MiB/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| single-threaded | 10527 | 10522 | 11001 | 2.58% | 3.132 |
| worker-event-loops | 10756 | 10518 | 10994 | 3.13% | 3.201 |
| batched-writes | 10628 | 10007 | 11097 | 5.17% | 3.162 |

## Instability Warning

- single-threaded/produce-throughput exceeded 15% throughput CV (27.13%) across 3 completed rounds; the limited campaign ended without the planned variance-extension rounds.
- worker-event-loops/produce-throughput exceeded 15% throughput CV (38.39%) across 2 completed rounds; the limited campaign ended without the planned variance-extension rounds.

## Every Measured Round

| Profile | Workload | Round | Records/s | MiB/s | Records | Errors | Append batches | Batch records |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| single-threaded | produce-throughput | 1 | 71.116 | 0.021 | 2142 | 0 | 50255 | 50255 |
| single-threaded | produce-throughput | 2 | 124.958 | 0.037 | 3750 | 0 | 35956 | 35956 |
| single-threaded | produce-throughput | 3 | 104.383 | 0.031 | 3148 | 0 | 31483 | 31483 |
| single-threaded | produce-latency | 1 | 47.026 | 0.014 | 100000 | 0 | 110000 | 110000 |
| single-threaded | produce-latency | 2 | 50.119 | 0.015 | 100000 | 0 | 110000 | 110000 |
| single-threaded | produce-latency | 3 | 46.431 | 0.014 | 100000 | 0 | 110000 | 110000 |
| single-threaded | fetch-throughput | 1 | 11001.372 | 3.273 | 250000 | 0 | 0 | 0 |
| single-threaded | fetch-throughput | 2 | 10522.436 | 3.131 | 250000 | 0 | 0 | 0 |
| single-threaded | fetch-throughput | 3 | 10526.729 | 3.132 | 250000 | 0 | 0 | 0 |
| worker-event-loops | produce-throughput | 1 | 67.109 | 0.020 | 2041 | 0 | 34302 | 34302 |
| worker-event-loops | produce-throughput | 2 | 117.113 | 0.035 | 3539 | 0 | 34634 | 34634 |
| worker-event-loops | produce-latency | 1 | 36.364 | 0.011 | 100000 | 0 | 110000 | 110000 |
| worker-event-loops | produce-latency | 2 | 43.201 | 0.013 | 100000 | 0 | 110000 | 110000 |
| worker-event-loops | fetch-throughput | 1 | 10518.175 | 3.130 | 250000 | 0 | 0 | 0 |
| worker-event-loops | fetch-throughput | 2 | 10994.093 | 3.271 | 250000 | 0 | 0 | 0 |
| batched-writes | produce-throughput | 1 | 190.829 | 0.057 | 5762 | 0 | 16820 | 62075 |
| batched-writes | produce-throughput | 2 | 179.814 | 0.054 | 5402 | 0 | 17162 | 64254 |
| batched-writes | produce-throughput | 3 | 171.158 | 0.051 | 5171 | 0 | 16666 | 61747 |
| batched-writes | produce-latency | 1 | 187.187 | 0.056 | 100000 | 0 | 28928 | 110000 |
| batched-writes | produce-latency | 2 | 186.389 | 0.055 | 100000 | 0 | 28827 | 110000 |
| batched-writes | produce-latency | 3 | 160.069 | 0.048 | 100000 | 0 | 28949 | 110000 |
| batched-writes | fetch-throughput | 1 | 10628.022 | 3.162 | 250000 | 0 | 0 | 0 |
| batched-writes | fetch-throughput | 2 | 11097.091 | 3.302 | 250000 | 0 | 0 | 0 |
| batched-writes | fetch-throughput | 3 | 10006.520 | 2.977 | 250000 | 0 | 0 | 0 |

Fetch setup is excluded from the timed result: each disposable topic is created through the authenticated broker, deterministically preloaded while the isolated broker is stopped using storage batches of 1,024, and then read and fully verified through four authenticated partition-specific consumers after the exact target profile restarts.

The original 100k/400k/750k targets remain aspirational developer-class goals, not Phase 10 completion gates. Full per-repetition results, batch counters, workload parameters, and dispersion data are stored in [the canonical JSON](../benchmarks/results/phase-10-gcp-e2-micro.json). Native Windows Release measurements are documented [separately](benchmarks-windows.md).
