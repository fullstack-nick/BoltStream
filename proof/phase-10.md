# Phase 10 Proof - Benchmarking and Performance Engineering

Date: 2026-07-11

Status: complete for deployed runtime commit `14d225abe1d5` under the
operator-approved limited GCP measurement scope described below.

## Scope And Boundary

Phase 10 adds reproducible performance tooling and three real runtime modes
without changing protocol version `4` or storage format version `2`:

- Configurable Asio I/O workers with per-session/client strands.
- Inline, worker-event-loop, and internally batched append profiles.
- Transactional `PartitionLog::append_batch` with contiguous offsets and rollback.
- Bounded append-batch and runtime-worker metrics.
- Authenticated end-to-end produce/fetch benchmarks and Google Benchmark
  microbenchmarks.
- Versioned raw/consolidated JSON and deterministic Markdown/README publication.

Durability is `flush`: responses follow C++ log and index stream flushes. Phase 10
does not claim `fsync` and does not add public batch-produce or compression.

## Local And CI Verification

Formatting passed and all 82 tests passed under native Windows MSVC, GCC, and
Clang builds. The tests cover configuration bounds and cross-fields, append order,
segment boundaries, rollback/recovery, strands and correlation, concurrent admin and
broker sessions, shutdown, retention, metrics, percentile/statistical helpers, and
the Phase 10 benchmark smoke.

The Phase 10 smoke additionally proved:

- All three checked-in runtime profiles start and complete all three workloads.
- Unbatched profiles emit exactly one record per append batch.
- `batched-writes` emits an average batch size above one.
- Direct fetch preparation creates exact per-partition counts and contiguous offsets.
- Non-empty direct preparation fails closed with exit `3`.
- End-to-end dry-run publishes no measurements and Google Benchmark dry-run passes.

Final GitHub Actions run `29146724323` passed for full commit
`14d225abe1d5e839e9ae13d0417dff9044cd0ef4`:

```text
Linux Debug: success (tests, formatting, benchmark smoke, operations assets)
Linux Release: success (packaged artifact)
Windows MSVC smoke: success (Phase 7, Phase 9, and Phase 10 smokes)
Linux ThreadSanitizer: success (focused concurrent broker tests)
```

CI does not gate on performance numbers from shared runners.

After the raw results and deterministic reports were checked in, publication CI run
`29154946121` passed for full commit
`cfe3f24ee875f074c2f44ff2ceb2aece4fd4a0ff`. That run re-proved Linux Debug and
Release, Windows MSVC, ThreadSanitizer, all Phase 10 smokes, and both GCP/Windows
report-regeneration checks. The subsequent proof commit changes documentation only.

## Exact Release Artifact

The exact CI artifact deployed to GCP contains `boltstream-server`,
`boltstream-bench`, and `boltstream-microbench`:

```text
artifact: artifacts/ci-14d225abe1d5/boltstream-linux-x86_64-14d225abe1d5.tar.gz
bytes: 2110284
sha256: 46D817516C80C13EC9B2142E689D3FAA1DDCEE2593258CCEA792E43BD0A455F0
CI run: 29146724323
```

The deployed `/version` response was:

```json
{"service":"boltstream","version":"0.1.0","git_sha":"14d225abe1d5","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"4","storage_format_version":"2"}
```

## GCP Methodology And Campaign Decision

The benchmark ran co-located client and broker processes over loopback on the
existing Ubuntu `e2-micro`. The observed CPU was AMD EPYC 7B12 with two logical
CPUs and 1,001,922,560 bytes RAM. The shared-core VM can burst, so these results
describe this exact live environment rather than dedicated-host capacity.

Headline workloads were unchanged:

- Produce throughput: four partitions, 16 connections, one outstanding request per
  connection, 256-byte values, 16-byte keys, 60-second warmup, 30 seconds measured.
- Produce latency: the same topology, 10,000 warmup messages, 100,000 measured
  acknowledged messages using `steady_clock` correlation latency.
- Fetch throughput: 250,000 deterministic records and four partition-specific
  authenticated consumers verifying every key, value, offset, and record.

Fetch setup is not reported as broker produce performance. Each disposable topic was
created through the authenticated broker; while the isolated profile was stopped,
`boltstream-bench prepare-fetch` wrote storage batches of 1,024; the exact target
profile was restarted before the authenticated timed read. Raw JSON labels the method
`direct-batched-storage-setup`.

The planned command was:

```powershell
.\deployments\gcp\scripts\benchmark-phase10-live.ps1 `
  -GitSha 14d225abe1d5 `
  -OutputDir artifacts/benchmarks/gcp-final-14d225abe1d5 `
  -Rounds 5
```

The operator stopped the campaign after approximately four and a half hours rather
than spend another day on the shared-core VM. At that point the runner had downloaded
two complete rotated rounds for every profile plus a third complete round for
`single-threaded` and `batched-writes`: 24 raw JSON files and eight complete
three-workload profile triplets. The interrupted `worker-event-loops` round three had
no complete local triplet and is excluded. Every completed triplet is retained; no
completed sample was selected away or overwritten.

This is explicitly a limited engineering campaign, not the originally planned
five-round capacity study. High variance is published as a warning. The complete raw
inputs are under `benchmarks/results/raw/gcp`, consolidated results are in
`benchmarks/results/phase-10-gcp-e2-micro.json`, and the deterministic report is
`docs/benchmarks.md`.

## GCP Results

All completed samples reported zero errors. Every latency sample acknowledged exactly
100,000 measured records, and every fetch sample verified exactly 250,000 records.

| Profile | Produce records/s median | Min | Max | CV | Latency p99 us | Fetch records/s median | Completed rounds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| single-threaded | 104.383 | 71.116 | 124.958 | 27.13% | 753321.145 | 10526.729 | 3 |
| worker-event-loops | 92.111 | 67.109 | 117.113 | 38.39% | 756699.618 | 10756.134 | 2 |
| batched-writes | 179.814 | 171.158 | 190.829 | 5.46% | 243553.735 | 10628.022 | 3 |

The measured recommendation is `batched-writes`. It improved median acknowledged
produce throughput and substantially reduced tail latency and variance in this live
environment. It remains an explicit benchmark profile; ordinary compiled, Compose,
and GCP defaults remain compatibility-oriented.

Batch metric evidence across completed produce workloads was:

```text
single-threaded:    447694 batches / 447694 records = 1.000
worker-event-loops: 288936 batches / 288936 records = 1.000
batched-writes:     137352 batches / 518076 records = 3.772
```

The original 100k/400k/750k figures remain aspirational developer-class goals. They
were not met on this shared-core VM. The observed bottleneck is per-record log/index
open-write-flush work in the unbatched profiles after VM/disk burst capacity is
exhausted. Internal batching improves this materially but does not turn `e2-micro`
into dedicated benchmark hardware. Missing those goals does not block Phase 10.

## Native Windows Secondary Results

The final-SHA native Windows Release matrix used four partitions, 16 clients,
256-byte values, 16-byte keys, and three repetitions. It intentionally used smaller
secondary workloads: three-second throughput windows after two-second warmups, 200
latency warmup plus 2,000 measured messages, and 5,000 authenticated-protocol preload
records for fetch.

| Profile | Produce records/s | Produce CV | Latency p99 us | Fetch records/s |
| --- | ---: | ---: | ---: | ---: |
| single-threaded | 132 | 7.31% | 161401.600 | 4080 |
| worker-event-loops | 193 | 30.13% | 164726.100 | 8003 |
| batched-writes | 1010 | 43.58% | 41206.700 | 8467 |

All nine Windows workloads reported zero errors. Exact configurations and every
repetition are retained in `benchmarks/results/phase-10-windows-release.json`; the
secondary report is `docs/benchmarks-windows.md`.

## Publication Reproducibility

The checked-in publication regenerates exactly with:

```powershell
.\scripts\render-benchmarks.ps1 `
  -InputDir benchmarks/results/raw/gcp -Check
.\scripts\render-windows-benchmarks.ps1 `
  -InputDir benchmarks/results/raw/windows -Check
```

Publication rejects Debug builds, mixed or missing SHAs/profiles/workloads, nonzero
errors, wrong fixed record counts, wrong protocol/storage versions, secrets, client
addresses, invalid batch metrics, and manual table edits. CI runs both regeneration
checks and never gates on numeric performance.

## Live Restoration And Cleanup

The benchmark used isolated `/var/lib/boltstream/phase10-*` data and temporary
systemd overrides. After stopping the limited campaign, cleanup was forced and then
verified:

```text
service: active
ExecStart: boltstream-server --config /etc/boltstream/boltstream.yaml
ready: 1
git_sha: 14d225abe1d5
runtime io workers: 1
runtime append workers: 2
data disk: 20G total, 19G available, 1% used
phase10 override/config/script/result/data paths on VM: absent
phase10 topics in normal data directory: absent
authenticated admin request after restoration: success
post-restoration journal error events: none
```

The active GCP account guard remained `nickaccturk@gmail.com`. Final
`terraform plan -detailed-exitcode -input=false -no-color` returned exit code `0`:

```text
No changes. Your infrastructure matches the configuration.
```

Phase 10 is therefore complete locally, in CI, in the exact packaged artifact, and
on live GCP under the explicitly limited measurement campaign. A future longer
capacity study can resume completed samples, but it is not required to proceed to
Phase 11.
