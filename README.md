# BoltStream

BoltStream is a C++20 low-latency event streaming engine: a Kafka-inspired broker built from scratch to demonstrate Linux networking, durable storage, concurrency, testing, performance measurement, and cloud-native deployment.

Phase 9 adds strict YAML configuration, Prometheus metrics, hardened health
contracts, structured operational logging, alerts, and a provisioned Grafana
dashboard on top of the Phase 8 retention and lifecycle runtime.

## Current Phase 9 Surface

- `boltstream-server` opens broker TCP port `9000`.
- Admin HTTP listens on `127.0.0.1:9100`.
- `GET /health/live` reports process liveness.
- `GET /health/ready` reports data-directory readiness.
- `GET /version` reports service name, Git SHA, build type, compiler, protocol version `4`, storage format version `2`, and startup time.
- `GET /metrics` exposes Prometheus text `0.0.4` for traffic, latency, connections,
  queues, storage, consumer lag, retention, recovery, and filesystem capacity.
- `--config`, `--check-config`, and `--print-effective-config` provide strict YAML
  configuration with CLI overrides and secret-free effective output.
- Broker TCP port `9000` accepts versioned binary frames with correlation ids and structured error responses.
- `boltstream-admin topics create` creates topics with an immutable partition count before produce.
- `boltstream-admin topics list|describe|delete` inspects and safely deletes topics through the broker.
- `boltstream-admin retention run` applies broker retention policy and reports deleted segments/bytes.
- `boltstream-admin groups describe|reset-offset` inspects and resets inactive group offsets.
- `boltstream-producer` appends durable records through the broker and prints assigned topic, partition, offset, and next offset.
- `boltstream-consumer` fetches durable records from `beginning`, `latest`, `committed`, or an explicit offset for a chosen partition.
- `boltstream-consumer --group GROUP --commit` commits the returned partition `next_offset` to a durable group offset log.
- `boltstream-consumer --coordinated --group GROUP --commit` joins a broker-managed group, receives automatic partition assignments, heartbeats, rejoins on rebalances, and commits offsets fenced by member generation.
- Long-poll fetch is available with `boltstream-consumer --wait-ms MS`.
- Append queues are bounded per partition and return retryable `overloaded` errors instead of growing without limit.
- Long-poll waiter state, broker sessions, frame sizes, fetch records, and fetch bytes are bounded by server options.
- Segment retention is configurable by age and size. Retention deletes inactive complete segments and exposes retained low watermarks.
- Fetching or committing retained-away offsets returns `offset_out_of_range`.
- Broker runtime logs are structured JSON lines with build identity, component,
  event names, correlation ids, error codes, retryable flags, queue/waiter state,
  and request duration in microseconds.
- `boltstream-logtool` remains available for direct append, read, and recovery inspection of durable records.
- `BOLTSTREAM_BROKER_TOKEN` enables broker-protocol auth; local development may omit it, while GCP deploys require it.
- `boltstream-bench` runs authenticated produce-throughput, acknowledged-latency,
  and fetch-throughput workloads with JSON and Markdown export.
- `boltstream-microbench` isolates protocol, append, batching, segment-roll, and
  read-path costs with Google Benchmark.

## Phase 10 Benchmark Results

<!-- PHASE10_BENCHMARK_START -->

Limited GCP `e2-micro` results for exact commit `14d225abe1d5` (two complete rounds for all profiles; a third for single-threaded and batched-writes). Measured recommendation: **batched-writes**.

| Profile | Median records/s | Min | Max | CV | Median MiB/s | p50 (us) | p95 (us) | p99 (us) | max (us) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| single-threaded | 104 | 71 | 125 | 27.13% | 0.031 | 258644.424 | 741374.534 | 753321.145 | 1992654.750 |
| worker-event-loops | 92 | 67 | 117 | 38.39% | 0.028 | 391006.978 | 746455.544 | 756699.619 | 1492236.219 |
| batched-writes | 180 | 171 | 191 | 5.46% | 0.054 | 12962.789 | 239372.586 | 243553.735 | 970660.144 |

See [docs/benchmarks.md](docs/benchmarks.md) for methodology, fetch results, dispersion, and the canonical JSON.

<!-- PHASE10_BENCHMARK_END -->

## Native Windows Build

The primary local development path is native CMake plus Ninja. The machine is configured with MSVC, MSYS2 UCRT GCC, LLVM Clang, CMake, Ninja, and Python 3.11.

```powershell
.\scripts\toolchain-check.ps1
.\scripts\build.ps1 -Preset windows-gcc-debug
.\scripts\test.ps1 -Preset windows-gcc-debug
.\scripts\build.ps1 -Preset windows-msvc-debug
.\scripts\build.ps1 -Preset windows-clang-debug
```

MSVC builds require the Visual Studio environment. The scripts call `vcvars64.bat` automatically for MSVC presets.

## Linux Parity Build

Docker remains the Linux parity path because GCP runs Linux.

```powershell
docker build --target builder -t boltstream-builder .
docker compose up --build -d
docker compose ps -a
curl.exe -fsS http://127.0.0.1:9090/api/v1/query?query=up
curl.exe -fsS http://127.0.0.1:3000/api/health
```

The Compose demo creates a topic, produces 25 records, consumes and commits the
first 10, leaves visible group lag, scrapes the broker with Prometheus, and
provisions the `BoltStream Operations` dashboard in Grafana. Host ports `9000`,
`9100`, `9090`, and `3000` bind to loopback only.

## Local Server Smoke

```powershell
.\build\windows-gcc-debug\boltstream-server.exe --config .\config\boltstream.example.yaml --listen 127.0.0.1:9000 --admin-listen 127.0.0.1:9100 --data .\data
curl.exe -fsS http://127.0.0.1:9100/health/live
curl.exe -fsS http://127.0.0.1:9100/health/ready
curl.exe -fsS http://127.0.0.1:9100/version
curl.exe -fsS http://127.0.0.1:9100/metrics
.\build\windows-gcc-debug\boltstream-admin.exe topics create --topic trades --partitions 3
.\build\windows-gcc-debug\boltstream-admin.exe topics describe --topic trades
.\build\windows-gcc-debug\boltstream-producer.exe --topic trades --key AAPL --message "AAPL,100,192.41"
.\build\windows-gcc-debug\boltstream-consumer.exe --topic trades --partition 0 --from beginning
.\build\windows-gcc-debug\boltstream-consumer.exe --topic trades --partition 0 --group dashboard --commit
.\build\windows-gcc-debug\boltstream-consumer.exe --topic trades --group dashboard --commit --coordinated
.\build\windows-gcc-debug\boltstream-admin.exe groups describe --group dashboard --topic trades
.\build\windows-gcc-debug\boltstream-admin.exe retention run --topic trades
.\build\windows-gcc-debug\boltstream-logtool.exe append --data .\data --topic trades --key AAPL --message "AAPL,100,192.41"
.\build\windows-gcc-debug\boltstream-logtool.exe read --data .\data --topic trades --from 0 --max-records 10
```

Use `curl.exe` in PowerShell. Plain `curl` is a PowerShell alias.
Producer output includes `partition`, `offset`, `next_offset`, and encoded byte size. Consumer output includes returned records, `next_offset`, and `committed_offset` when `--commit` is used.

For a repeatable local smoke:

```powershell
.\scripts\smoke-phase5.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase6.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase7.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase8.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase9.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase4.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase3.ps1 -Preset windows-gcc-debug
```

See [docs/protocol.md](docs/protocol.md) for the binary frame layout and
[docs/storage.md](docs/storage.md) for the durable log format.

## GCP Deployment

BoltStream is deployed to a dedicated GCP project:

- Project: `boltstream-r7m5o9ld`
- Account guard: `nickaccturk@gmail.com`
- Region: `us-central1`
- Zone: `us-central1-a`
- Runtime: Terraform-managed `e2-micro` Compute Engine VM
- Deploy control: direct SSH plus systemd
- Data path: `/var/lib/boltstream`

See [docs/gcp.md](docs/gcp.md) and [docs/operations.md](docs/operations.md).

## Phase Proof

Durable acceptance records live under `proof/`. Phase 1 is recorded in
[proof/phase-1.md](proof/phase-1.md), and Phase 2 is recorded in
[proof/phase-2.md](proof/phase-2.md). Phase 3 is recorded in
[proof/phase-3.md](proof/phase-3.md) after local checks, GitHub push, CI artifact,
GCP deploy, live storage calls, SSH log/data-file inspection, and runtime version
verification pass. Phase 4 evidence is recorded in [proof/phase-4.md](proof/phase-4.md).
Phase 5 evidence is recorded in [proof/phase-5.md](proof/phase-5.md). Phase 6 evidence
is recorded in [proof/phase-6.md](proof/phase-6.md). Phase 7 evidence is recorded in
[proof/phase-7.md](proof/phase-7.md) after live coordinated group proof. Phase 8 evidence
is recorded in [proof/phase-8.md](proof/phase-8.md) after live retention and lifecycle proof.
Phase 9 evidence is recorded in [proof/phase-9.md](proof/phase-9.md) after exact-artifact
GCP metrics, operations-stack, structured-log, alert, dashboard, and cleanup proof.
