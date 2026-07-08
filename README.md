# BoltStream

BoltStream is a C++20 low-latency event streaming engine: a Kafka-inspired broker built from scratch to demonstrate Linux networking, durable storage, concurrency, testing, performance measurement, and cloud-native deployment.

Phase 2 establishes the broker/client binary protocol and C++ async client library. Durable storage and real produce/fetch success paths are intentionally left for later phases.

## Current Phase 2 Surface

- `boltstream-server` opens broker TCP port `9000`.
- Admin HTTP listens on `127.0.0.1:9100`.
- `GET /health/live` reports process liveness.
- `GET /health/ready` reports data-directory readiness.
- `GET /version` reports service name, Git SHA, build type, compiler, protocol version `1`, storage format version `0`, and startup time.
- Broker TCP port `9000` accepts versioned binary frames with correlation ids and structured error responses.
- `boltstream-producer` and `boltstream-consumer` use the C++ async client library and return structured `not_implemented` responses until durable produce/fetch behavior lands.
- `boltstream-bench` remains a stable benchmark shell until real produce/fetch support exists.

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
.\build\windows-gcc-debug\boltstream-producer.exe --topic trades --key AAPL --message "AAPL,100,192.41"
.\build\windows-gcc-debug\boltstream-consumer.exe --topic trades --from beginning
```

Use `curl.exe` in PowerShell. Plain `curl` is a PowerShell alias.

Producer and consumer currently return exit code `3` with `"status":"not_implemented"`.
That is the expected Phase 2 result because storage-backed produce/fetch lands later.

For a repeatable local smoke:

```powershell
.\scripts\smoke-phase2.ps1 -Preset windows-gcc-debug
```

See [docs/protocol.md](docs/protocol.md) for the binary frame layout.

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
[proof/phase-2.md](proof/phase-2.md) after local checks, GitHub push, CI artifact, GCP
deploy, live protocol calls, SSH log inspection, and runtime version verification pass.
