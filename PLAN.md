# BoltStream Plan

## Product Definition

BoltStream is a C++20 low-latency event streaming engine: a GCP-deployed, production-style event log broker inspired by Kafka, built from scratch to demonstrate high-performance systems engineering, Linux networking, concurrency, durable storage, benchmarking, observability, cloud deployment, and operational robustness.

The finished project is not a toy message queue and not a localhost-only demo. It is a complete broker that runs locally and live on Google Cloud Platform, with a clear path to multi-broker replication, crash recovery, client interoperability, measurable latency, and production-style tooling.

BoltStream is cloud-native in the practical systems-engineering sense: every feature built locally must also be deployable, observable, and testable on the live GCP environment. The GCP target is intentionally low-level and controlled: Terraform-managed infrastructure, a directly SSH-managed Compute Engine VM, explicit systemd/runtime control, live logs, live data files, and live protocol calls.

Portfolio title:

> BoltStream - C++20 Low-Latency Event Streaming Engine

Portfolio one-liner:

> Built a Kafka-inspired event streaming broker in C++20 with TCP networking, append-only log storage, consumer offsets, benchmarks, Docker, CI, metrics, replication simulation, compression, and crash-recovery tests.

## Success Criteria

BoltStream is complete when the repository proves all of the following:

- A broker accepts concurrent producer and consumer clients over TCP.
- Messages are stored durably in append-only segmented logs.
- Topics support configurable partitions.
- Every appended message receives a stable topic, partition, and offset.
- Consumers can read from `beginning`, `latest`, or an explicit offset.
- Consumer groups commit and resume offsets.
- Coordinated consumer groups support membership, heartbeats, automatic partition assignment, and rebalance behavior.
- Retention and topic lifecycle tooling prevent unbounded disk growth.
- The server restarts cleanly and rebuilds metadata from disk.
- Crash recovery tests prove that partial writes do not corrupt committed data.
- Benchmarks report throughput plus p50, p95, and p99 latency.
- Backpressure prevents unbounded memory growth under slow consumers or overloaded producers.
- Metrics expose broker, storage, client, and replication health.
- Docker and CI run the main build, tests, and smoke checks.
- README documentation presents commands, architecture, benchmark evidence, and recovery proof.
- Every completed phase is pushed to GitHub, deployed to GCP, and verified against the live GCP broker.
- The live GCP runtime exposes the exact Git commit SHA that is deployed.
- Every feature has live proof: call the live GCP broker, verify the expected client-visible result, then inspect systemd status, logs, metrics, and relevant data files over SSH.
- No product feature, demo path, proof path, benchmark path, or operational script remains local-only.

## Command-Line Experience

Primary commands:

```bash
boltstream-server --port 9000 --data ./data
boltstream-producer --topic trades --key AAPL --message "AAPL,100,192.41"
boltstream-consumer --topic trades --from beginning
boltstream-consumer --topic trades --group dashboard --from latest
boltstream-bench --topic trades --producers 4 --consumers 2 --messages 1000000 --payload-bytes 256
```

The CLI tools must work against the same binary TCP protocol used by all clients. The command-line path is the first public demonstration path, not a separate shortcut around the broker.

## Locked Design Decisions

These decisions are the implementation baseline. They stay locked unless explicitly changed before coding.

### Platform

- Primary target: Linux x86_64.
- Development support: Windows host through native CMake/Ninja toolchains, with Docker retained for Linux parity and GCP behavior treated as authoritative.
- Language standard: C++20.
- Build system: CMake.
- Test framework: GoogleTest.
- Benchmark framework: Google Benchmark plus a custom end-to-end latency harness.
- Formatting and static checks: `clang-format`, `clang-tidy`, compiler warnings as errors in CI.
- Package strategy: CMake FetchContent for project dependencies, with dependency versions pinned.

### Local Native C++ Toolchain

The Windows development machine has a native C++ toolchain installed and verified. Docker remains useful for Linux parity, but it is not the only local build path.

Verified after Codex relaunch on 2026-07-07:

- CMake `4.3.4` at `C:\Program Files\CMake\bin\cmake.exe`.
- Ninja `1.13.2` at `C:\Users\napet\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe`.
- LLVM `22.1.8` under `C:\Program Files\LLVM\bin`, including:
  - `clang++.exe`
  - `clang-format.exe`
  - `clang-tidy.exe`
  - `clangd.exe`
  - `lldb.exe`
  - `llvm-cov.exe`
  - `llvm-profdata.exe`
- Visual Studio Build Tools 2022 with C++ workload and Windows SDK under `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`.
- MSVC environment script at `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat`.
- MSVC compiler after `vcvars64.bat`: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe`.
- MSVC compiler version: `19.44.35228`.
- MSYS2 UCRT64 toolchain rooted at `C:\msys64\ucrt64\bin`, including:
  - GCC `16.1.0` at `C:\msys64\ucrt64\bin\g++.exe`
  - GDB `17.2` at `C:\msys64\ucrt64\bin\gdb.exe`
  - MSYS2 CMake, Ninja, pkgconf, make, curl, and zstd packages
- MSYS2 archive helpers under `C:\msys64\usr\bin`, including `zip.exe`, `unzip.exe`, and `tar.exe`.
- Python `3.11.9` at `C:\Users\napet\AppData\Local\Programs\Python\Python311\python.exe`.
- Python launcher supports `py -3.11`.

Verification completed:

- `g++`, `clang++`, and MSVC `cl` each compiled and ran a C++20 test program.
- CMake plus Ninja built a C++20 test project with GCC and MSVC; direct compiler checks also passed with Clang.
- `lldb` starts correctly after Python `3.11.9` was installed so LLVM can load `python311.dll`.
- User `PATH` is updated for fresh terminals and fresh Codex sessions.
- In PowerShell, use `curl.exe` for real curl because `curl` alone resolves to a PowerShell alias.
- For MSVC builds, load the Visual Studio environment first with `vcvars64.bat` or Developer PowerShell for VS 2022.

### GCP Cloud-Native Deployment

- BoltStream is designed for GCP deployment from the first phase.
- Budget boundary: stay inside Google Cloud Free Tier or active Free Trial credits. Before any Terraform apply, verify current official GCP limits because cloud pricing and eligibility can change.
- Current free-tier deployment target: one non-preemptible `e2-micro` Compute Engine VM in a supported US free-tier region, initially `us-central1-a`, with standard persistent disk kept within the free-tier storage allowance.
- Primary runtime host: Ubuntu 24.04 LTS on Compute Engine, matching CI and Docker Linux parity builds.
- Primary storage path: `/var/lib/boltstream` on persistent disk.
- Primary install path: `/opt/boltstream/releases/<git-sha>` with `/opt/boltstream/current` pointing to the active release.
- Runtime manager: systemd service named `boltstream.service`.
- Infrastructure owner: Terraform.
- Operational control plane: direct SSH using `gcloud compute ssh`.
- Deployment control: SSH deploy scripts that install the CI-built Linux release artifact for the exact pushed GitHub commit, restart systemd, and verify the deployed SHA.
- Live broker port: TCP `9000`.
- Live metrics/admin access: localhost by default, inspected through SSH tunnels; public exposure requires explicit authentication and Terraform-managed firewall rules.
- Core broker behavior must not depend on managed Pub/Sub, managed Kafka, Cloud SQL, GKE, or Cloud Run. Those services hide the systems work this project is meant to demonstrate and can violate the cost/control goal.
- GCP is not a final packaging afterthought. Every phase must have local verification and live GCP verification before it counts as implemented.

### GitHub and Deployment Gate

- `main` is the source-of-truth branch unless a task explicitly says otherwise.
- A phase is not implemented until the relevant commit is pushed to GitHub.
- CI must pass before the deployed phase is called complete.
- The live VM must run the same Git SHA that was pushed and tested.
- Dirty-worktree local binaries are not deployable artifacts.
- Deployment scripts must fail closed if the repo has uncommitted changes, the commit is not pushed, CI has failed, or the active GCP project/account is not the expected BoltStream target.
- Runtime version endpoints and logs must include build type, Git SHA, protocol version, storage format version, and startup time.

### Live Verification Gate

Every phase inherits this gate:

1. Run local build, tests, and targeted smoke checks.
2. Commit the implementation and proof updates.
3. Push to GitHub.
4. Confirm CI status for the pushed commit.
5. Apply Terraform if infrastructure changed.
6. Deploy the pushed commit to the GCP VM.
7. Call the live GCP broker or live metrics endpoint from outside the process.
8. SSH into the VM and inspect `systemctl status boltstream`, `journalctl -u boltstream`, metrics, config, runtime version, and relevant data files.
9. Prove the live result matches the feature expectation, not only that the process is running.
10. Record the exact local commands, live commands, commit SHA, VM name, external address, and observed result in the phase proof artifact.

If a feature changes storage, replication, metrics, compression, recovery, or backpressure behavior, the live proof must inspect the corresponding on-disk files, logs, counters, or replicated state on the VM.

### No Local-Only Rule

- Anything built for BoltStream must have a live GCP path.
- Local-only implementations are not accepted as finished work.
- Local developer helpers are acceptable only when they also support or verify the live GCP path.
- Benchmarks must run locally and have a smaller live-GCP profile that respects the free-tier VM.
- Crash recovery tests must have local automation and a live GCP proof mode.
- Documentation must describe both local and live GCP usage for every user-facing feature.

### Security and Access Control

- The live broker must not expose unauthenticated write access.
- Broker clients authenticate with a configured token from the first live produce/fetch phase.
- Admin and metrics endpoints are restricted to localhost or trusted source ranges unless explicitly protected and documented.
- Firewall rules are Terraform-managed and narrow by default.
- SSH is used for operational inspection and live proof, not as a hidden product shortcut around the broker protocol.
- Live broker secrets are stored in GCP Secret Manager and materialized into root-owned environment files during deploy; plaintext secrets are never committed.

### Networking

- Transport: TCP.
- Protocol: custom binary protocol with explicit frame length, version, request type, correlation id, flags, and payload.
- Concurrency model: acceptor thread plus worker event loops using non-blocking sockets.
- Implementation baseline: Boost.Asio, wrapped behind BoltStream-owned interfaces.
- Framing: length-prefixed frames with a maximum request size enforced by configuration.
- Client model: one TCP connection can send multiple produce/fetch/commit requests using correlation ids.
- Error handling: every protocol error returns a structured response and closes the connection only for malformed or unsafe frames.

### Protocol

Frame header:

```text
uint32 magic
uint16 version
uint16 frame_type
uint32 header_bytes
uint32 payload_bytes
uint64 correlation_id
uint32 flags
uint32 header_crc32
```

Core request types:

- `ProduceRequest`
- `ProduceResponse`
- `FetchRequest`
- `FetchResponse`
- `OffsetCommitRequest`
- `OffsetCommitResponse`
- `MetadataRequest`
- `MetadataResponse`
- `HealthRequest`
- `HealthResponse`
- `AuthRequest`
- `AuthResponse`

Serialization format:

- Binary protocol for broker/client traffic.
- Fixed-width integers use network byte order.
- Strings and byte arrays use length-prefix encoding.
- Message payloads are opaque bytes.
- Protocol compatibility is versioned from the first implementation.

### Topics and Partitions

- A topic contains one or more partitions.
- Default partitions per topic: `1`.
- Partition count is configured at topic creation time.
- Partition selection uses key hash when a key is provided.
- Partition selection uses round-robin when no key is provided.
- Offsets are monotonically increasing per topic partition.
- Topic names are validated with a strict portable character set: `[a-zA-Z0-9._-]`.

### Storage

- Storage model: append-only log segment files.
- Directory layout:

```text
data/
  topics/
    trades/
      partition-000000/
        00000000000000000000.log
        00000000000000000000.index
        00000000000000000000.timeindex
        manifest.json
  consumer_offsets/
    group-name/
      offsets.log
  broker.meta
```

- Segment naming: base offset as a zero-padded decimal string.
- Segment rolling: roll by size and by age.
- Default segment size: `256 MiB`.
- Index file maps logical offsets to physical file positions.
- Time index maps timestamps to nearest offsets.
- Message record format:

```text
uint32 record_bytes
uint16 record_version
uint16 flags
uint64 offset
uint64 timestamp_unix_ns
uint32 key_bytes
uint32 value_bytes
uint32 header_count
bytes  key
bytes  value
bytes  headers
uint32 record_crc32
```

- CRC covers the record body and detects torn or corrupted writes.
- Recovery scans segments, truncates incomplete trailing records, rebuilds indexes, and verifies manifests.
- Writes use batching to reduce syscall and fsync overhead.

### Durability

- A produce response means the record has been appended to the in-memory batch and accepted by the storage pipeline.
- Durability level is explicit per request:
  - `acks=memory`: acknowledged after broker accepts the batch.
  - `acks=flush`: acknowledged after bytes are flushed to the OS.
  - `acks=fsync`: acknowledged after `fsync` completes.
- Default durability: `acks=flush`.
- Benchmark reports must state the durability mode used.
- Crash recovery tests focus on `acks=fsync` and `acks=flush`.

### Consumer Model

- Consumers can fetch from `beginning`, `latest`, or an explicit offset.
- Consumer groups are first-class.
- Offset commits are written to durable offset logs.
- Coordinated consumer groups use broker-managed membership, heartbeats, generation ids, automatic partition assignment, and rebalance behavior after the partition and offset foundation is stable.
- Consumers use pull-based fetch requests.
- Fetch supports max bytes, max records, and long-poll timeout.
- Slow consumers are isolated from producers by bounded queues and fetch limits.

### Backpressure

- All queues are bounded.
- Producer connections receive retryable overload errors when partition append queues are full.
- Consumer fetches are capped by response byte size and server-side in-flight limits.
- The broker exposes metrics for queue depth, rejected requests, append latency, and fetch latency.
- Overload behavior is deterministic and tested.

### Topic Lifecycle and Retention

- Topics are created explicitly before multi-partition produce.
- Topic partition count is immutable after creation.
- Operators can list, describe, and delete topics through checked-in tooling.
- Segment retention is configurable by age and size.
- Retention deletes only complete inactive segments and preserves recovery safety.
- Fetching from an offset that has aged out returns a structured offset-out-of-range response.
- Live proof must inspect disk usage, manifests, segment deletion, and topic lifecycle results.

### Replication Simulation

- Replication is part of the committed product plan.
- The first replication target is a localhost multi-broker mode.
- Each partition has a leader and follower replicas.
- Followers fetch from the leader using the same internal record format.
- Produce acknowledgement supports:
  - `replication=leader`
  - `replication=all`
- Replication health is visible in metadata and metrics.
- The project demonstrates leader/follower behavior without claiming full Kafka-grade cluster membership.

### Compression

- Compression is part of the committed product plan.
- Supported compression modes:
  - `none`
  - `zstd`
- Compression is applied per produce batch.
- Fetch responses preserve compressed batches when the client supports the compression codec.
- Benchmarks include uncompressed and zstd-compressed modes.

### Metrics and Observability

- Metrics endpoint: HTTP on a separate port.
- Format: Prometheus-compatible text exposition.
- Required metrics:
  - broker uptime
  - active connections
  - produce request rate
  - fetch request rate
  - append latency histogram
  - fetch latency histogram
  - bytes in/out
  - records in/out
  - partition queue depth
  - segment count and active segment bytes
  - recovery duration
  - rejected requests
  - consumer group members, generations, rebalances, heartbeat failures, and commit failures
  - retention-deleted segments and retained bytes
  - replication lag
- Logs are structured and include correlation ids.

### Performance Targets

Initial public benchmark targets on a local developer machine:

| Mode | Throughput Target | p50 Latency Target | p99 Latency Target |
| --- | ---: | ---: | ---: |
| Single-threaded broker | 100k msg/s | 1.5 ms | 8.0 ms |
| Worker event loops | 400k msg/s | 1.0 ms | 5.0 ms |
| Batched writes | 750k msg/s | 0.8 ms | 3.0 ms |
| Batched writes + zstd | 300k msg/s | 2.0 ms | 10.0 ms |

Benchmark numbers in README must be produced by checked-in benchmark commands and labeled with CPU, OS, compiler, build type, payload size, durability mode, client count, and date.

### Testing Strategy

Required test categories:

- Unit tests for protocol encoding and decoding.
- Unit tests for record serialization and CRC validation.
- Unit tests for segment append, read, roll, and index rebuild.
- Broker integration tests using real TCP clients.
- Consumer offset commit and resume tests.
- Coordinated consumer group membership, heartbeat, generation, assignment, rebalance, and stale commit tests.
- Retention and topic lifecycle tests for segment deletion, topic deletion, offset-out-of-range behavior, and manifest recovery.
- Backpressure tests with bounded queues.
- Crash recovery tests that kill the broker during writes.
- Replication tests with leader and follower brokers.
- Compression compatibility tests.
- CLI smoke tests.
- Docker smoke tests.

### Documentation

The repository will contain:

- `README.md` for recruiter-facing overview, quickstart, screenshots, benchmark table, and architecture.
- `PLAN.md` for this implementation plan.
- `docs/protocol.md` for binary protocol details.
- `docs/storage.md` for log and recovery details.
- `docs/admin.md` for topic lifecycle, retention, and consumer group operations.
- `docs/benchmarks.md` for reproducible benchmark methodology and results.
- `docs/operations.md` for running, tuning, metrics, and recovery.
- `docs/gcp.md` for Terraform, SSH deployment, live verification, free-tier limits, and cost guardrails.
- `proof/phase-N.md` files for durable local and live GCP acceptance evidence.

Only `PLAN.md` is created in this step.

## Implementation Roadmap

Every phase below inherits the GitHub and Deployment Gate, the Live Verification Gate, and the No Local-Only Rule. Local build/test success is necessary but never sufficient.

### Phase 1: Repository Foundation

Deliverables:

- CMake project layout.
- Native Windows build support for GCC, Clang, and MSVC.
- CMake presets for native Windows and Linux-parity builds.
- PowerShell build, test, format, and toolchain-check scripts.
- Server, producer CLI, consumer CLI, and benchmark CLI targets.
- GoogleTest integration.
- CI build matrix for Linux debug and release.
- CI-produced Linux release artifact for SSH deployment.
- Formatting and lint scripts.
- Dockerfile and docker-compose demo skeleton for Linux parity, not as the primary local build path.
- Terraform skeleton for the GCP free-tier VM, firewall, service account, and persistent disk.
- SSH deployment scripts.
- Live smoke and live inspection scripts.
- Runtime version endpoint that exposes the deployed Git SHA.

Acceptance:

- Clean configure/build/test cycle.
- Native CMake plus Ninja build works on Windows with GCC, Clang, and MSVC.
- Docker Linux-parity build succeeds and uses the same CMake targets as native and CI builds.
- CI runs the same core checks as local development.
- Empty broker starts, accepts health requests, and shuts down cleanly.
- Terraform provisions the first GCP VM inside the free-tier/free-trial budget.
- The empty broker is deployed to GCP through SSH, responds to a live health request, and has clean systemd logs after the request.

#### Detailed Phase 1 Implementation Plan

Summary:

- Build the first shippable BoltStream foundation: public GitHub repo, C++20/CMake project skeleton, native Windows build path, Docker Linux-parity path, empty broker runtime, health/version endpoints, CI artifact pipeline, Terraform-managed GCP free-tier VM, SSH deployment scripts, and live proof on GCP.
- Phase 1 is complete only when the exact pushed GitHub commit is built locally, built by CI, deployed to the dedicated GCP VM, verified through live calls plus SSH inspection, and documented in `proof/phase-1.md`.

Key implementation changes:

- Initialize Git cleanly: remove the invalid empty `.git`, run `git init -b main`, create public `fullstack-nick/BoltStream`, add `.gitignore`, and keep `.agents`, `.codex`, build outputs, Terraform state, local tfvars, secrets, and generated archives untracked.
- Create the C++ foundation with CMake, C++20, warning-as-error targets, GoogleTest, Google Benchmark, and Boost.Asio through pinned CMake FetchContent dependencies.
- Add `CMakePresets.json` for these local paths:
  - `windows-msvc-debug`
  - `windows-msvc-release`
  - `windows-gcc-debug`
  - `windows-clang-debug`
  - `linux-gcc-debug`
  - `linux-gcc-release`
- Make native Windows builds the primary local development path. Use the installed MSVC, MSYS2 UCRT GCC, and LLVM Clang toolchains documented above.
- Keep Docker as a Linux-parity path that mirrors CI and GCP behavior. The Docker image must use Ubuntu 24.04 with CMake, Ninja, GCC, Clang tooling, curl, and packaging tools.
- Add these initial binaries:
  - `boltstream-server`: starts TCP broker listener on `--listen 0.0.0.0:9000`, admin HTTP listener on `--admin-listen 127.0.0.1:9100`, and data path from `--data`.
  - `boltstream-producer`, `boltstream-consumer`, and `boltstream-bench`: support `--help` and return a clear `protocol starts in Phase 2` error for real operations.
- Add Phase 1 admin HTTP interface:
  - `GET /health/live`: process is running.
  - `GET /health/ready`: config parsed and data directory exists and is writable.
  - `GET /version`: JSON with service name, Git SHA, build type, compiler, protocol version `0`, storage format version `0`, and startup time.
- Add CI:
  - Ubuntu 24.04 Debug and Release configure/build/test jobs.
  - Linux release artifact upload named `boltstream-linux-x86_64-${sha}.tar.gz`.
  - Windows native smoke build with MSVC to protect the local development path.
  - Formatting check and clang-tidy check.
- Add GCP deployment foundation:
  - Dedicated project `boltstream-r7m5o9ld`, billing account `010A7B-134BD2-8CB391`, account guard `nickaccturk@gmail.com`.
  - Region `us-central1`, zone `us-central1-a`.
  - Terraform-managed VPC, firewall, service account, Ubuntu 24.04 LTS VM, 10 GB boot disk, and 20 GB standard persistent data disk mounted at `/var/lib/boltstream`.
  - Direct SSH from operator `/32`; broker port `9000` restricted to operator `/32`; admin port `9100` localhost-only.
  - GCS Terraform backend bucket `boltstream-r7m5o9ld-tfstate` with lifecycle cleanup for old versions.
  - Secret Manager metadata for future broker/admin tokens, with secret payloads created outside Terraform state.
- Add SSH/systemd deployment:
  - Install artifacts under `/opt/boltstream/releases/<git-sha>`.
  - Symlink `/opt/boltstream/current`.
  - Run as a dedicated `boltstream` Linux user.
  - Manage with `boltstream.service`.
  - Materialize root-owned env/config files under `/etc/boltstream`.
- Add proof and docs for Phase 1: `README.md`, `docs/gcp.md`, `docs/operations.md`, and `proof/phase-1.md`.

Test plan:

- Local native verification:
  - `scripts/toolchain-check.ps1` confirms CMake, Ninja, GCC, Clang, MSVC, clang-format, clang-tidy, lldb, Python 3.11, and archive tools.
  - CMake configure/build succeeds with `windows-msvc-debug`, `windows-gcc-debug`, and `windows-clang-debug`.
  - CMake configure/build succeeds with `windows-msvc-release`.
  - `ctest --output-on-failure` passes for the native Debug build.
  - `boltstream-server` starts locally, creates or uses a temp data dir, and serves `/health/live`, `/health/ready`, and `/version`.
  - CLI tools print stable `--help`.
  - Benchmark target supports a dry-run placeholder without publishing performance numbers.
- Docker Linux-parity verification:
  - Docker builder image builds.
  - CMake configure/build/test succeeds in the Linux container.
- CI verification:
  - GitHub Actions passes on the pushed commit.
  - Linux release artifact is uploaded and named with the commit SHA.
  - Windows MSVC smoke build passes.
- Terraform verification:
  - Bootstrap confirms active account, billing account, project id, region, zone, free-tier disk sizing, and operator `/32`.
  - `terraform fmt -check`, `terraform validate`, `terraform plan`, and `terraform apply` succeed.
  - Post-apply `terraform plan -detailed-exitcode` returns clean state.
- Live GCP verification:
  - Deploy CI artifact for the exact pushed SHA.
  - `systemctl status boltstream` is active.
  - `journalctl -u boltstream` shows clean startup and no errors after live calls.
  - SSH call to `127.0.0.1:9100/health/live`, `/health/ready`, and `/version` returns expected JSON and matching Git SHA.
  - External operator machine can reach TCP `9000`; non-operator access is blocked by firewall.
  - SSH inspection confirms `/var/lib/boltstream` is mounted, owned correctly, and writable by the service user.
  - `proof/phase-1.md` records commit SHA, CI run URL, GCP project, VM name, zone, commands run, live outputs, and final go/no-go result.

Assumptions and locked defaults:

- GitHub target: public `fullstack-nick/BoltStream`.
- GCP target: new dedicated `boltstream-r7m5o9ld` project under `nickaccturk@gmail.com`.
- Access model: direct SSH limited to current operator `/32`; IAP is not part of Phase 1.
- Terraform state: remote GCS backend after a bootstrap step creates the bucket.
- Free-tier posture: stay within one `e2-micro`, 30 GB total standard persistent disk, narrow firewall rules, and minimal network egress.
- Native Windows builds are the primary local development path.
- Docker is the Linux-parity path and must remain available because GCP runs Linux.
- MSVC builds require `vcvars64.bat` or Developer PowerShell for VS 2022.
- PowerShell live/proof scripts must call `curl.exe`, not the `curl` alias.
- Admin HTTP is for health/version only in Phase 1; broker protocol implementation starts in Phase 2.
- Sources to re-check during implementation: [Google Cloud Free Program](https://docs.cloud.google.com/free/docs/free-cloud-features), [Compute Engine Terraform docs](https://docs.cloud.google.com/compute/docs/terraform), [Terraform GCS backend](https://developer.hashicorp.com/terraform/language/backend/gcs), and [GitHub Actions runner images](https://github.com/actions/runner-images).

### Phase 2: Binary Protocol and Client Library

Deliverables:

- Versioned frame header.
- Request/response encoding and decoding.
- Correlation id handling.
- Protocol error responses.
- C++ client library used by CLI tools.

Acceptance:

- Protocol tests cover valid frames, truncated frames, invalid lengths, unsupported versions, and malformed payloads.
- Producer and consumer CLIs communicate through the client library.

### Phase 3: Durable Single-Partition Log

Deliverables:

- Append-only segment writer.
- Segment reader.
- Offset assignment.
- CRC validation.
- Index rebuild on restart.
- Recovery truncation for incomplete trailing records.

Acceptance:

- Messages survive broker restart.
- Corrupted trailing data is detected and truncated.
- Storage tests prove append, read, roll, and recovery behavior.

### Phase 4: Broker Produce and Fetch

Deliverables:

- TCP broker accepts concurrent clients.
- Produce requests append records to topic partitions.
- Fetch requests read records by offset.
- Topic metadata endpoint.
- Initial bounded queues and request limits.

Acceptance:

- Multiple producers and consumers operate concurrently.
- CLI workflow works:

```bash
boltstream-server --port 9000 --data ./data
boltstream-producer --topic trades --key AAPL --message "AAPL,100,192.41"
boltstream-consumer --topic trades --from beginning
```

### Phase 5: Multi-Partition Topics and Consumer Groups

Deliverables:

- Explicit create-topic protocol and admin CLI with immutable partition count.
- Topic manifest with partition count.
- Key-hash and round-robin partition selection.
- Consumer group offset log.
- Offset commit and resume.
- Long-poll fetch.
- Manual partition selection for consumers.

Acceptance:

- Topics are created explicitly before multi-partition produce.
- Consumers resume from committed offsets after restart.
- Partition distribution is deterministic and tested.
- Long-poll fetch avoids tight polling loops.

### Phase 6: Backpressure and Robustness

Deliverables:

- Bounded append queues.
- Per-connection in-flight request limits.
- Max frame size and max fetch response size enforcement.
- Deterministic overload responses.
- Structured logging with correlation ids.
- Locked defaults:
  - `--max-append-queue-depth 32` per partition; `0` is valid for deterministic overload proof.
  - `--append-workers 2`.
  - `--max-broker-connections 128`.
  - `--max-long-poll-waiters 128`; `0` rejects waiting fetches while immediate fetch still works.
- Retryable protocol error `overloaded = 16` with CLI error JSON field `"retryable": true` and exit code `5`.
- `FetchResponse.next_offset` is the consumer resume offset, not the partition high watermark when a response is truncated.

Acceptance:

- Slow consumers do not cause unbounded memory growth.
- Overloaded producers receive retryable errors.
- Abuse cases are covered by integration tests.
- Local checks, CI artifact, GCP deploy, live authenticated smoke, constrained live overload proof, structured journal evidence, and `proof/phase-6.md` are required before the phase is complete.

### Phase 7: Coordinated Consumer Groups

Deliverables:

- Broker-side group coordinator.
- Join, leave, heartbeat, and sync-group style protocol flow.
- Member ids, generation ids, session timeouts, and stale member fencing.
- Automatic partition assignment across active group members.
- Rebalance behavior when members join, leave, crash, or time out.
- Offset commits fenced by group generation.

Acceptance:

- Consumers in the same group divide topic partitions automatically.
- Rebalances are deterministic and tested for join, leave, timeout, and restart cases.
- Stale members cannot commit offsets for an old generation.
- Live GCP proof shows two consumers sharing partitions, then one consumer taking over after the other stops heartbeating.

### Phase 8: Retention and Topic Lifecycle

Deliverables:

- Configurable segment retention by age and size.
- Safe deletion of inactive retained segments.
- Topic list, describe, and delete commands in `boltstream-admin`.
- Consumer group describe and reset-offset commands.
- Offset-out-of-range responses when retained data no longer contains the requested offset.
- Operational documentation for disk growth and cleanup.

Acceptance:

- Retention frees disk without corrupting active partitions or recovery.
- Topic deletion removes manifests, partition logs, and related metadata safely.
- Admin lifecycle commands work locally and against the live GCP broker.
- Live proof records before/after disk and file-state inspection.

### Phase 9: Metrics and Operations

Deliverables:

- Prometheus-compatible metrics endpoint.
- Health endpoint.
- Broker runtime configuration file.
- Operational logging.
- Docker compose demo with broker, producer, consumer, and metrics scraping path.

Acceptance:

- Metrics expose traffic, latency, storage, queue, and error state.
- A local operator can diagnose throughput, slow consumers, and disk growth from metrics and logs.

### Phase 10: Benchmarking and Performance Engineering

Deliverables:

- Throughput benchmark.
- Latency benchmark with p50, p95, p99, and max.
- Single-threaded mode.
- Worker event loop mode.
- Batched write mode.
- Benchmark result export as Markdown and JSON.

Acceptance:

- README benchmark table is generated from reproducible commands.
- Benchmarks state hardware, OS, compiler, build type, payload size, durability mode, and client counts.

### Phase 11: Compression

Deliverables:

- zstd batch compression.
- Compression negotiation in protocol metadata.
- Compressed produce and fetch path.
- Compression benchmark mode.

Acceptance:

- zstd and uncompressed clients interoperate according to negotiated capabilities.
- Benchmarks compare throughput, latency, and bytes written.

### Phase 12: Replication Simulation

Deliverables:

- Local multi-broker mode.
- Partition leader/follower assignment.
- Follower replication fetch loop.
- Replication lag metric.
- `replication=leader` and `replication=all` produce modes.

Acceptance:

- A follower catches up from a leader.
- Produce acknowledgements respect replication mode.
- Restarted follower resumes from its last replicated offset.

### Phase 13: Crash Recovery Proof

Deliverables:

- Automated kill-during-write tests.
- Recovery proof script.
- Durable documentation of recovery behavior.
- Tests for torn records, partial batches, and index rebuild.

Acceptance:

- Crash tests run from a clean checkout.
- Recovered logs contain exactly the committed valid records.
- README includes the recovery proof command and result summary.

### Phase 14: Recruiter-Grade Polish

Deliverables:

- README with architecture diagram, quickstart, demo commands, metrics example, benchmark table, and recovery proof.
- Protocol and storage docs.
- Docker demo.
- GCP deployment docs and live proof docs.
- CI badge.
- Small interoperability client in Python.

Acceptance:

- A reviewer can clone, build, run locally, deploy to GCP with documented Terraform/SSH steps, produce, consume, inspect metrics, run tests, and reproduce a benchmark without private setup beyond their own GCP project credentials.
- The project clearly communicates C++20, Linux, networking, concurrency, storage, testing, performance, and production habits.

## Repository Structure

Target layout:

```text
boltstream/
  CMakeLists.txt
  README.md
  PLAN.md
  Dockerfile
  docker-compose.yml
  cmake/
  config/
    boltstream.example.yaml
  deployments/
    gcp/
      terraform/
      scripts/
        bootstrap.ps1
        deploy.ps1
        smoke-live.ps1
        inspect-live.ps1
  docs/
    protocol.md
    storage.md
    admin.md
    benchmarks.md
    operations.md
    gcp.md
  proof/
    phase-1.md
    phase-2.md
  include/
    boltstream/
      client/
      protocol/
      storage/
      broker/
      metrics/
  src/
    broker/
    client/
    protocol/
    storage/
    metrics/
    common/
  tools/
    producer_cli.cpp
    consumer_cli.cpp
    bench_cli.cpp
    admin_cli.cpp
  tests/
    unit/
    integration/
    crash/
  benchmarks/
    throughput_bench.cpp
    latency_bench.cpp
  scripts/
    format.ps1
    test.ps1
    bench.ps1
    crash_recovery.ps1
  examples/
    python_client/
```

## Pre-Implementation Research Checklist

Before implementation starts, validate and document these choices:

- Re-check official Google Cloud Free Tier and Free Trial limits for Compute Engine, disk, network egress, regions, and billing behavior before provisioning.
- Confirm the first GCP region/zone, with `us-central1-a` as the proposed locked answer while it remains free-tier eligible.
- Confirm Terraform resources for VM, VPC, firewall, disk, service account, OS Login or SSH keys, and budget alerts.
- Confirm the deployment path for exact Git SHA CI artifacts installed over SSH.
- Confirm the runtime version endpoint and log fields needed to prove the deployed commit.
- Confirm the live smoke and SSH inspection command set that every phase will reuse.
- Confirm Boost.Asio packaging, CMake integration, and pinned version.
- Confirm zstd CMake integration strategy and pinned version.
- Confirm Linux file I/O strategy for append batching, flush, and fsync behavior.
- Confirm benchmark methodology against common low-latency C++ expectations.
- Review current C++ systems job listings for recurring terms to mirror honestly in README language.
- Check Prometheus text exposition requirements for correct metric formatting.
- Check GitHub Actions Linux image compiler versions for C++20 support.
- Check whether IAP, direct SSH firewall rules, or OS Login gives the best control/cost/security tradeoff for this project.

## External Research Notes

- [Google Cloud Free Program documentation](https://docs.cloud.google.com/free/docs/free-cloud-features) says Free Tier usage has monthly limits and usage above those limits can be billed.
- Current Compute Engine Free Tier documentation lists one non-preemptible `e2-micro` VM instance per month in `us-west1`, `us-central1`, or `us-east1`, plus 30 GB-months standard persistent disk and 1 GB outbound data transfer from North America to most destinations.
- Current Secret Manager Free Tier documentation lists 6 active secret versions and 10,000 access operations per month.
- [Compute Engine product documentation](https://cloud.google.com/products/compute) summarizes the free tier as one `e2-micro` VM, up to 30 GB standard persistent disk, and up to 1 GB outbound data transfer per month.
- These constraints must be verified again at bootstrap time and before any infrastructure expansion.

## Decision Questions With Locked Answers

These decisions have locked default answers. Before coding, confirm each one against the toolchain, free-tier GCP constraints, and live deployment shape; if the plan is not explicitly changed, implement the proposed answer.

1. Should the broker use Boost.Asio or standalone Asio?
   - Proposed locked answer: Boost.Asio, because it is widely recognized in C++ networking roles and integrates reliably with production-style async TCP.

2. Should the first storage implementation use plain buffered file I/O, `mmap`, or Linux `io_uring`?
   - Proposed locked answer: buffered file I/O with explicit batching, flush, and fsync controls; add `mmap` for read-path optimization after correctness and recovery are proven.

3. Should produce acknowledgements default to maximum safety or higher throughput?
   - Proposed locked answer: default to `acks=flush`, expose `acks=memory` and `acks=fsync`, and require every benchmark to state its durability mode.

4. Should BoltStream implement consumer groups from the start or only offset reads?
   - Proposed locked answer: implement consumer groups as part of the core plan, because durable resume behavior is essential for a useful event streaming broker.

5. Should configuration be CLI-only or file-backed?
   - Proposed locked answer: support both, with CLI flags overriding a YAML configuration file.

6. Should replication be real enough to demonstrate leader/follower mechanics or only simulated in tests?
   - Proposed locked answer: implement localhost multi-broker leader/follower replication with real TCP replication traffic and explicit replication lag metrics.

7. Should compression be delayed until after all broker basics work?
   - Proposed locked answer: implement compression after benchmark and metrics foundations, because compression results are only meaningful once the measurement path is reliable.

8. Should the project include a non-C++ client?
   - Proposed locked answer: include a Python demo client after the binary protocol stabilizes to prove interoperability without expanding the production support matrix.

9. Should HTTP be used for client operations?
   - Proposed locked answer: no. Client operations use the custom binary TCP protocol. HTTP is reserved for metrics and health endpoints.

10. Should the README claim Kafka compatibility?
    - Proposed locked answer: no. The project is Kafka-inspired, not Kafka-compatible. The value is original systems implementation, not protocol cloning.

11. What is the primary GCP runtime shape?
    - Proposed locked answer: one Terraform-managed Compute Engine `e2-micro` VM in a free-tier eligible US region, direct SSH control, systemd-managed BoltStream binary, persistent disk for `/var/lib/boltstream`, and narrow firewall rules.

12. What counts as a phase being implemented?
    - Proposed locked answer: local checks pass, commit is pushed to GitHub, CI passes, the exact commit is deployed to GCP, the live broker is called, SSH inspection verifies logs/metrics/storage/runtime state, and a durable proof artifact records the result.

13. Can a feature be considered done if it only works locally?
    - Proposed locked answer: no. Every feature, script, benchmark, demo, and proof path must be available against live GCP in a free-tier-safe profile.

14. Should deployment favor managed convenience or direct operational control?
    - Proposed locked answer: direct control. Use Terraform for infrastructure and SSH/systemd for deploy, inspection, logs, data files, metrics, and recovery proof.

15. How should public access be handled on the live VM?
    - Proposed locked answer: broker writes require authentication, admin/metrics stay localhost or trusted-source only, and Terraform-managed firewall rules stay narrow by default.

16. When should Kafka-style coordinated consumer groups be implemented?
    - Proposed locked answer: after Phase 6 backpressure and robustness, before metrics and benchmarking. Phase 5 implements the partition and durable-offset foundation; Phase 7 adds membership, heartbeats, automatic assignment, generation fencing, and rebalancing.

17. Does the product need retention and lifecycle tooling before it is complete?
    - Proposed locked answer: yes. Add a dedicated phase after coordinated consumer groups and before metrics so disk growth, topic deletion, group inspection, and offset reset behavior are real product features instead of ad hoc operator cleanup.

## README Outcome Target

The final README should show this feature summary:

```text
BoltStream: A C++20 low-latency event streaming engine

Features:
- TCP broker with custom binary protocol
- Append-only durable log storage
- Topic partitions and stable offsets
- Consumer groups with durable offset commits and coordinated rebalancing
- Topic retention, lifecycle, and admin tooling
- Free-tier-conscious GCP deployment with Terraform, SSH, and systemd
- Per-phase live GCP proof tied to pushed GitHub commits
- Multi-threaded producer/consumer pipeline
- Bounded queues and backpressure
- Prometheus-compatible metrics endpoint
- zstd batch compression
- Local leader/follower replication simulation
- Crash-recovery tests
- Benchmarks: throughput, p50, p95, p99 latency
- Dockerized demo and CI-tested with GoogleTest
```

Benchmark table target:

| Mode | Throughput | p50 Latency | p99 Latency |
| --- | ---: | ---: | ---: |
| Single-threaded | measured | measured | measured |
| Worker event loops | measured | measured | measured |
| Batched writes | measured | measured | measured |
| Batched writes + zstd | measured | measured | measured |

No benchmark number is published without a reproducible command and environment details.
