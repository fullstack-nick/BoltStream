# BoltStream

BoltStream is a C++20 low-latency event streaming engine: a Kafka-inspired broker built from scratch to demonstrate Linux networking, durable storage, concurrency, testing, performance measurement, and cloud-native deployment.

Phase 1 establishes the repository foundation: native Windows builds, Linux parity builds, an empty broker listener, admin health/version endpoints, CI artifacts, Terraform-managed GCP infrastructure, and SSH/systemd deployment.

## Current Phase 1 Surface

- `boltstream-server` opens broker TCP port `9000`.
- Admin HTTP listens on `127.0.0.1:9100`.
- `GET /health/live` reports process liveness.
- `GET /health/ready` reports data-directory readiness.
- `GET /version` reports service name, Git SHA, build type, compiler, protocol version `0`, storage format version `0`, and startup time.
- `boltstream-producer`, `boltstream-consumer`, and `boltstream-bench` provide stable CLI shells; real produce/fetch protocol starts in Phase 2.

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
```

Use `curl.exe` in PowerShell. Plain `curl` is a PowerShell alias.

## GCP Deployment

BoltStream is deployed from Phase 1 to a dedicated GCP project:

- Project: `boltstream-r7m5o9ld`
- Account guard: `nickaccturk@gmail.com`
- Region: `us-central1`
- Zone: `us-central1-a`
- Runtime: Terraform-managed `e2-micro` Compute Engine VM
- Deploy control: direct SSH plus systemd
- Data path: `/var/lib/boltstream`

See [docs/gcp.md](docs/gcp.md) and [docs/operations.md](docs/operations.md).

## Phase 1 Proof

The durable acceptance record is [proof/phase-1.md](proof/phase-1.md). Phase 1 is complete only after local checks, GitHub push, CI artifact, Terraform apply, GCP deploy, live health/version calls, SSH log inspection, and clean post-apply Terraform drift check all pass.

