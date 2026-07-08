# BoltStream

BoltStream is a C++20 low-latency event streaming engine: a Kafka-inspired broker built from scratch to demonstrate Linux networking, durable storage, concurrency, testing, performance measurement, and cloud-native deployment.

Phase 5 adds explicit topic creation, manifest-backed multi-partition topics,
broker-side partition routing, durable consumer group offset commits, committed
resume, and long-poll fetch over the binary TCP protocol.

## Current Phase 5 Surface

- `boltstream-server` opens broker TCP port `9000`.
- Admin HTTP listens on `127.0.0.1:9100`.
- `GET /health/live` reports process liveness.
- `GET /health/ready` reports data-directory readiness.
- `GET /version` reports service name, Git SHA, build type, compiler, protocol version `2`, storage format version `2`, and startup time.
- Broker TCP port `9000` accepts versioned binary frames with correlation ids and structured error responses.
- `boltstream-admin topics create` creates topics with an immutable partition count before produce.
- `boltstream-producer` appends durable records through the broker and prints assigned topic, partition, offset, and next offset.
- `boltstream-consumer` fetches durable records from `beginning`, `latest`, `committed`, or an explicit offset for a chosen partition.
- `boltstream-consumer --group GROUP --commit` commits the returned partition `next_offset` to a durable group offset log.
- Long-poll fetch is available with `boltstream-consumer --wait-ms MS`.
- `boltstream-logtool` remains available for direct append, read, and recovery inspection of durable records.
- `BOLTSTREAM_BROKER_TOKEN` enables broker-protocol auth; local development may omit it, while GCP deploys require it.
- `boltstream-bench` remains a stable benchmark shell until the benchmark phase.

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
docker compose up --build
```

## Local Server Smoke

```powershell
.\build\windows-gcc-debug\boltstream-server.exe --listen 127.0.0.1:9000 --admin-listen 127.0.0.1:9100 --data .\data
curl.exe -fsS http://127.0.0.1:9100/health/live
curl.exe -fsS http://127.0.0.1:9100/health/ready
curl.exe -fsS http://127.0.0.1:9100/version
.\build\windows-gcc-debug\boltstream-admin.exe topics create --topic trades --partitions 3
.\build\windows-gcc-debug\boltstream-producer.exe --topic trades --key AAPL --message "AAPL,100,192.41"
.\build\windows-gcc-debug\boltstream-consumer.exe --topic trades --partition 0 --from beginning
.\build\windows-gcc-debug\boltstream-consumer.exe --topic trades --partition 0 --group dashboard --commit
.\build\windows-gcc-debug\boltstream-logtool.exe append --data .\data --topic trades --key AAPL --message "AAPL,100,192.41"
.\build\windows-gcc-debug\boltstream-logtool.exe read --data .\data --topic trades --from 0 --max-records 10
```

Use `curl.exe` in PowerShell. Plain `curl` is a PowerShell alias.
Producer output includes `partition`, `offset`, `next_offset`, and encoded byte size. Consumer output includes returned records, `next_offset`, and `committed_offset` when `--commit` is used.

For a repeatable local smoke:

```powershell
.\scripts\smoke-phase5.ps1 -Preset windows-gcc-debug
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
