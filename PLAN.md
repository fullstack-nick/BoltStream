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

Initial aspirational targets for developer-class hardware; these guide engineering
but are not Phase 10 completion gates or expectations for the shared-core GCP VM:

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
- Locked scope: coordinated groups operate on one topic per `(group, topic)`
  coordinator instance. Multi-topic subscriptions are intentionally out of the
  committed roadmap.

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
- Protocol version `4` lifecycle frames for topic list/describe/delete, retention run,
  group describe, and group offset reset.
- Broker options `--segment-bytes`, `--segment-max-age-seconds`,
  `--retention-max-age-seconds`, `--retention-max-bytes`, and
  `--retention-check-interval-ms`.
- Default retention policy: 7 day max age, 1 GiB max bytes per partition, one hour
  segment max age, and a 60 second retention timer.

Acceptance:

- Retention frees disk without corrupting active partitions or recovery.
- Topic deletion removes manifests, partition logs, and related metadata safely.
- Admin lifecycle commands work locally and against the live GCP broker.
- Live proof records before/after disk and file-state inspection.
- Topic deletion and group offset reset reject active coordinated groups.
- Fetching `beginning` starts at the retained low watermark; explicit and committed
  offsets below it return `offset_out_of_range`.

### Phase 9: Metrics and Operations

Phase goal: turn the Phase 8 broker into an observable and operable service without
changing the binary client protocol or moving the data plane onto a managed service.
Phase 9 reuses the existing localhost admin listener and structured JSON log stream,
then gives them stable contracts, configuration, dashboards, alerts, runbooks, and a
live GCP proof path.

#### Locked Phase 9 Boundary

- The existing admin HTTP listener remains the only HTTP server. It serves
  `/health/live`, `/health/ready`, `/version`, and the new `/metrics` endpoint.
- Prometheus text exposition is implemented directly in BoltStream. Do not add a
  second HTTP framework or the `prometheus-cpp` dependency for this bounded metric
  set.
- YAML parsing uses `yaml-cpp` release `0.9.0`, pinned by immutable tag and source
  hash through CMake `FetchContent`.
- Prometheus `3.13.0` LTS and Grafana `13.1.0` are pinned for the operator-side
  Docker Compose stack. Their image digests are recorded when implementation starts.
- Prometheus and Grafana run on the operator machine for the local demo and for live
  inspection through an SSH tunnel. They do not become resident services on the
  free-tier `e2-micro` VM.
- The GCP admin listener remains bound to `127.0.0.1:9100`. Phase 9 does not add a
  public metrics firewall rule or public unauthenticated operations surface.
- Metrics are in-memory operational state. Counters reset when the broker restarts;
  Prometheus handles rates and reset detection.
- Phase 9 does not implement benchmark result publication, compression, replication,
  OpenTelemetry tracing, log aggregation, or Alertmanager notification delivery.
  Those boundaries preserve Phases 10 through 12 and keep the VM inside its cost and
  resource envelope.

#### Deliverables

- Concurrency-safe counters, gauges, and fixed-bucket histograms owned by a dedicated
  `boltstream::observability` module.
- `GET /metrics` on the existing admin listener, rendered as Prometheus text format
  `0.0.4` with deterministic `HELP` and `TYPE` lines, escaped label values, base
  units, cumulative histogram buckets, no sample timestamps, and a trailing newline.
- Stable health contracts for `/health/live` and `/health/ready`, with explicit HTTP
  status behavior and readiness state reflected in metrics.
- Strict YAML runtime configuration with `--config`, `--check-config`, and
  `--print-effective-config`, while preserving existing CLI flags as overrides.
- Centralized JSON Lines operational logging with level filtering, stable field
  names, correlation ids, event names, request durations, and secret/payload
  redaction.
- Runtime snapshots for current topic, partition, consumer group, queue, retention,
  recovery, and filesystem state without blocking the broker hot path for the full
  duration of an HTTP scrape.
- Docker Compose demo containing BoltStream, topic initialization, producer,
  consumer, Prometheus, and Grafana, with loopback-only host port publishing and
  persistent local Prometheus/Grafana data volumes.
- Checked-in Prometheus scrape configuration, alert rules, Grafana provisioning, and
  one BoltStream operations dashboard.
- Operator commands and runbooks for throughput, latency, overload, slow consumers,
  disk growth, retention failures, recovery, safe restart, configuration validation,
  and live GCP inspection.
- `scripts/smoke-phase9.ps1`, a live metrics tunnel helper, Phase 9 additions to the
  existing GCP deploy/smoke/inspect scripts, and durable `proof/phase-9.md` evidence.

#### Metrics Architecture

- Add `include/boltstream/observability/metrics.h` and
  `src/observability/metrics.cpp` with three explicit responsibilities:
  - `MetricsRegistry` owns monotonic process-lifetime counters and fixed-bucket
    histograms. Hot-path metric labels are closed enums such as operation and error
    code, so request processing never inserts arbitrary strings into a map.
  - `RuntimeMetricsSnapshot` is an immutable value assembled for each scrape from
    current broker, topic, partition, group, offset, recovery, retention, and
    filesystem state.
  - `PrometheusTextRenderer` renders one registry snapshot plus one runtime snapshot
    in deterministic metric-family and label order.
- Pass one registry instance from `BrokerServer` into `BrokerRuntime` and each
  `BrokerProtocolSession`; do not use file-scope mutable metrics or independent
  registries.
- Global counters and histogram bucket counts use thread-safe numeric storage.
  Dynamic topic/group gauges are collected as immutable scrape-time values rather
  than stored forever in the registry, so deleting a topic or group removes its
  series on the next scrape.
- Snapshot locking follows this order:
  1. copy topic `shared_ptr` values while holding `topics_mutex_`, then release it;
  2. inspect one topic and partition at a time using the existing topic, append, and
     log locks;
  3. collect offset and group snapshots through new read-only snapshot APIs;
  4. release all broker locks before rendering text.
- A scrape never performs retention, mutates offsets, expires members, or waits for
  append queues to drain. Concurrent scrapes and live broker traffic must be safe.
- Histograms use seconds and cumulative buckets at `0.0005`, `0.001`, `0.0025`,
  `0.005`, `0.01`, `0.025`, `0.05`, `0.1`, `0.25`, `0.5`, `1`, `2.5`, `5`, `10`,
  `30`, `60`, and `+Inf`. The upper buckets cover the configured long-poll path.
- Metric and label names remain ASCII snake case. Correlation ids, member ids,
  client endpoints, record keys, record values, tokens, and free-form error messages
  are forbidden as metric labels.
- `topic`, `partition`, and `group` labels are restricted to current resource gauges
  where they are needed to diagnose queue depth, disk use, or consumer lag. Counter
  labels use only finite operation, error-code, direction, and reason vocabularies.

#### Locked Metric Catalog

| Metric | Type and labels | Meaning |
| --- | --- | --- |
| `boltstream_build_info` | gauge; build metadata labels | Constant `1` with version, Git SHA, build type, compiler, protocol version, and storage format version. |
| `boltstream_uptime_seconds` | gauge | Seconds since the broker startup timestamp. |
| `boltstream_ready` | gauge | `1` only while recovery is complete, the data directory is writable, and shutdown has not started. |
| `boltstream_connections_active` | gauge | Current accepted broker protocol sessions. |
| `boltstream_connections_accepted_total` | counter | Broker sessions accepted since startup. |
| `boltstream_connections_rejected_total{reason}` | counter | Connections rejected for a bounded reason such as `limit`. |
| `boltstream_requests_total{operation}` | counter | Decoded broker requests by finite protocol operation. |
| `boltstream_request_errors_total{operation,error_code}` | counter | Structured protocol failures, separate from total requests so error ratios remain direct. |
| `boltstream_request_duration_seconds{operation}` | histogram | End-to-end broker processing time, including append queue time and fetch long-poll time. |
| `boltstream_protocol_received_bytes_total` | counter | Frame header plus payload bytes received from broker clients. |
| `boltstream_protocol_sent_bytes_total` | counter | Frame header plus payload bytes sent to broker clients. |
| `boltstream_records_produced_total` | counter | Records durably appended through successful produce requests. |
| `boltstream_records_fetched_total` | counter | Records returned in successful fetch responses. |
| `boltstream_partition_append_queue_depth{topic,partition}` | gauge | Active plus pending append work for a current partition. |
| `boltstream_partition_append_queue_capacity{topic,partition}` | gauge | Configured per-partition append queue limit. |
| `boltstream_long_poll_waiters` | gauge | Current registered long-poll fetch waiters. |
| `boltstream_rejected_requests_total{operation,reason}` | counter | Deterministic overload and limit rejections such as append queue or long-poll capacity. |
| `boltstream_topics` | gauge | Current topic count. |
| `boltstream_partitions` | gauge | Current partition count across all topics. |
| `boltstream_partition_segments{topic,partition}` | gauge | Current segment count for each partition. |
| `boltstream_partition_log_bytes{topic,partition}` | gauge | Current retained log bytes for each partition. |
| `boltstream_partition_earliest_offset{topic,partition}` | gauge | Current retained low watermark. |
| `boltstream_partition_next_offset{topic,partition}` | gauge | Current partition high watermark. |
| `boltstream_storage_capacity_bytes` | gauge | Filesystem capacity containing the configured data directory. |
| `boltstream_storage_available_bytes` | gauge | Bytes available to the broker on that filesystem. |
| `boltstream_storage_recovery_duration_seconds` | gauge | Duration of the most recent startup recovery in this process. |
| `boltstream_storage_recovered_records` | gauge | Records observed during startup recovery. |
| `boltstream_storage_truncated_bytes` | gauge | Corrupt or partial tail bytes truncated during startup recovery. |
| `boltstream_consumer_group_members{group,topic}` | gauge | Current active coordinated members for a current group/topic pair. |
| `boltstream_consumer_group_generation{group,topic}` | gauge | Current coordinator generation. |
| `boltstream_consumer_group_lag_records{group,topic,partition}` | gauge | `next_offset - committed_offset` for in-range durable group offsets; out-of-range offsets use the current earliest offset as the safe lag baseline. |
| `boltstream_consumer_group_offset_out_of_range{group,topic,partition}` | gauge | `1` when a durable group offset is below the retained low watermark or above the high watermark, otherwise `0`. |
| `boltstream_consumer_group_rebalances_total` | counter | Generation-changing rebalances since startup. |
| `boltstream_consumer_group_heartbeat_failures_total` | counter | Rejected or expired heartbeat operations since startup. |
| `boltstream_consumer_group_commit_failures_total` | counter | Fenced, invalid, or out-of-range group commits since startup. |
| `boltstream_retention_runs_total` | counter | Manual, scheduled, startup, and append-triggered retention passes. |
| `boltstream_retention_failures_total` | counter | Retention passes that failed. |
| `boltstream_retention_deleted_segments_total` | counter | Complete inactive segments deleted since startup. |
| `boltstream_retention_deleted_bytes_total` | counter | Log bytes deleted by retention since startup. |
| `boltstream_retention_retained_bytes{topic,partition}` | gauge | Current bytes retained after the latest scrape-time partition snapshot. |
| `boltstream_metrics_scrapes_total` | counter | `/metrics` requests served since startup. |
| `boltstream_metrics_render_duration_seconds` | histogram | Time to collect and render the previous completed metrics scrapes. |
| `boltstream_metrics_render_failures_total` | counter | Metrics snapshots or renders that returned an HTTP `500`. |

#### Metrics Endpoint Contract

- Parse the bounded admin request line into method, path, and query-free route instead
  of extracting only the path.
- `GET /metrics` returns `200`, `Content-Type: text/plain; version=0.0.4;
  charset=utf-8`, `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, a
  correct `Content-Length`, and `Connection: close`.
- The existing health and version routes remain JSON. `/health/live` returns `200`
  while the admin event loop can answer. `/health/ready` returns `200` only when
  `boltstream_ready` is `1`, otherwise `503` with the current readiness detail.
- Non-`GET` admin requests return `405` with `Allow: GET`; unknown paths return `404`.
  Oversized or malformed request lines return `400` and never reach the renderer.
- Rendering failure returns `500`, increments
  `boltstream_metrics_render_failures_total`, and writes a structured
  `metrics_render_failed` event without leaking a partial response body.
- `metrics.enabled` defaults to `true`. When explicitly set to `false`, the broker
  skips hot-path metric updates and returns `404` for `/metrics`; health and version
  routes remain available.
- The output contains exactly one `HELP` and `TYPE` declaration per family and emits
  cumulative histogram `_bucket`, `_sum`, and `_count` samples. Unit tests feed the
  result to `promtool check metrics` in the Linux/Compose verification path.

#### Runtime Configuration Contract

- Replace the current documentation-only `config/boltstream.example.yaml` with the
  actual accepted schema. The top-level mappings are `server`, `storage`,
  `retention`, `limits`, `metrics`, and `logging`.
- Canonical keys are:
  - `server.listen`, `server.admin_listen`;
  - `storage.data_dir`, `storage.segment_bytes`,
    `storage.segment_max_age_seconds`;
  - `retention.max_age_seconds`, `retention.max_bytes`,
    `retention.check_interval_ms`;
  - `limits.max_frame_bytes`, `limits.max_fetch_records`,
    `limits.max_fetch_bytes`, `limits.max_topic_partitions`,
    `limits.max_fetch_wait_ms`, `limits.max_append_queue_depth`,
    `limits.append_workers`, `limits.max_broker_connections`, and
    `limits.max_long_poll_waiters`;
  - `metrics.enabled`;
  - `logging.level` with `debug`, `info`, `warn`, or `error`, and
    `logging.format` locked to `json`.
- Build, phase, protocol, and storage format versions are compiled metadata and are
  removed from runtime configuration.
- Add `include/boltstream/config/config.h` and `src/config/config.cpp`. Parsing is a
  two-pass merge: compiled defaults, then YAML, then explicitly supplied CLI flags.
  `BOLTSTREAM_BROKER_TOKEN` remains the only secret input and is never accepted from
  the YAML file.
- `--config PATH` loads a file. Existing deployments that omit it keep compiled
  defaults. `--check-config` validates and exits before binding sockets or touching
  the data directory. `--print-effective-config` prints deterministic YAML with
  secret state represented only as `auth_required: true|false`.
- Unknown top-level or nested keys, duplicate keys, wrong scalar types, invalid
  endpoints, invalid enum values, integer overflow, and out-of-range limits fail
  closed with the full key path and YAML line/column where available. Configuration
  errors exit with code `2`.
- All existing flags remain supported and override file values. `--help` documents
  precedence, and startup emits `config_loaded` with the path and non-secret
  effective settings.
- Check in separate non-secret runtime files for the Docker demo and GCP service.
  Docker binds the admin listener to the Compose network while publishing it only on
  host loopback. GCP binds the admin listener to VM loopback.

#### Operational Logging Contract

- Move the anonymous `StructuredLogFields` and writer out of `server.cpp` into
  `include/boltstream/observability/logger.h` and
  `src/observability/logger.cpp`.
- Each line is one valid JSON object written atomically to `stderr` for Docker and
  journald capture. Required fields are `timestamp`, `level`, `event`, `component`,
  and `git_sha`; request events add correlation id, operation, duration in
  microseconds, error code, retryable state, and relevant bounded numeric state.
- Convert retention, recovery, topic lifecycle, group, overload, config, metrics,
  startup, and shutdown details from concatenated message strings into typed JSON
  fields. Keep a human-readable `message` only when it adds information not already
  represented by fields.
- Log-level filtering occurs before JSON allocation. `debug` is available for local
  diagnosis, while checked-in Docker and GCP configurations use `info`.
- Broker tokens, record keys, record values, payload bodies, environment contents,
  and config file contents are never logged. Proof artifacts omit client IPs and
  other operator-specific endpoints.
- Preserve correlation ids end to end for protocol request, response, and error
  events. Log successful request duration once at completion and failure duration
  once at the error response, avoiding double-counted latency metrics.

#### Docker Compose, Prometheus, Grafana, and Alerts

- Extend the runtime image to contain `boltstream-admin` as well as the server,
  producer, consumer, benchmark shell, and log tool. Add a Docker health check
  against `/health/ready`.
- Expand `docker-compose.yml` with these services:
  - `boltstream`: config-file-driven broker with persistent data and loopback-only
    host ports `9000` and `9100`;
  - `topic-init`: one-shot admin client that waits for readiness and creates the demo
    topic idempotently;
  - `producer`: deterministic one-shot traffic generator using the shipped producer
    CLI;
  - `consumer`: one-shot durable group consumer that intentionally leaves measurable
    lag, followed by a documented catch-up command;
  - `prometheus`: pinned `3.13.0` LTS image, persistent TSDB volume, a 5-second demo
    scrape interval, checked-in rules, and loopback-only port `9090`;
  - `grafana`: pinned `13.1.0` image, provisioned Prometheus datasource and dashboard,
    persistent data volume, and loopback-only port `3000`.
- Add `deployments/metrics/prometheus.yml`, `alerts.yml`, `alerts.test.yml`, Grafana
  datasource/dashboard provisioning, and the BoltStream dashboard JSON. Do not
  require manual UI setup.
- The dashboard contains broker identity/readiness, uptime, connections, record and
  byte rates, request errors, p50/p95/p99 request latency, append queue depth versus
  capacity, long-poll waiters, partition bytes/segments/watermarks, filesystem free
  ratio, group members/lag, rebalances, retention deletion, and recovery panels.
- Checked-in Prometheus alerts are:
  - `BoltStreamBrokerDown`: scrape target absent for one minute;
  - `BoltStreamNotReady`: readiness is `0` for two minutes;
  - `BoltStreamAppendOverload`: rejected append rate is nonzero for two minutes;
  - `BoltStreamConsumerLagHigh`: maximum group lag exceeds `10000` records for five
    minutes;
  - `BoltStreamDiskNearlyFull`: available/capacity ratio remains below `0.15` for five
    minutes;
  - `BoltStreamRetentionFailure`: a retention failure occurs in the last five
    minutes.
- Validate Prometheus configuration and rules with `promtool check config` and
  `promtool check rules`. Use `promtool test rules` with checked-in synthetic series
  to prove every alert's pending, firing, and cleared behavior without adding a
  five-minute wait to the smoke path. Query the Prometheus HTTP API during smoke
  tests to prove that the target is up, metric series are ingested, documented
  queries work, and the production rule group is loaded.

#### File-Level Implementation Map

- `CMakeLists.txt`: fetch pinned `yaml-cpp`, compile the config and observability
  sources, install real config examples, and register new tests.
- `include/boltstream/config/config.h`, `src/config/config.cpp`: schema, merge,
  validation, check, and redacted effective-config rendering.
- `include/boltstream/observability/metrics.h`,
  `src/observability/metrics.cpp`: registry, snapshots, histogram implementation,
  Prometheus renderer, escaping, and deterministic output.
- `include/boltstream/observability/logger.h`,
  `src/observability/logger.cpp`: JSON logger, severity filter, stable fields, and
  secret-safe rendering.
- `include/boltstream/broker/server.h`, `src/broker/server.cpp`: registry ownership,
  instrumentation points, runtime snapshot creation, bounded admin request parsing,
  `/metrics`, and finalized health semantics.
- `include/boltstream/broker/group_coordinator.h` and implementation: read-only
  current group/member/generation snapshots and event counters without expiring or
  mutating members during scrape.
- `include/boltstream/storage/offset_store.h` and implementation: read-only snapshot
  of all durable group offsets for lag calculation.
- `include/boltstream/storage/partition_log.h` and implementation: expose immutable
  segment/log-byte/watermark state already derivable from segment summaries; no
  metrics-specific storage writes.
- `src/tools/server_main.cpp` and broker option parsing: two-pass config/CLI handling,
  check/print modes, and documented exit codes.
- `tests/config_tests.cpp`: defaults, full schema, precedence, invalid/unknown keys,
  numeric bounds, YAML locations, secret exclusion, and deterministic redacted
  output.
- `tests/metrics_tests.cpp`: counters, concurrent increments, histogram bucket
  cumulative behavior, label/help escaping, family ordering, disappearing dynamic
  series, and golden Prometheus text.
- `tests/client_broker_tests.cpp`: live admin HTTP status/header behavior, traffic and
  error counter deltas, queue/storage/group/retention snapshots, recovery metrics,
  concurrent traffic plus repeated scrapes, and clean shutdown.
- `config/boltstream.example.yaml`, a Compose runtime config, and
  `deployments/gcp/boltstream.yaml`: checked-in non-secret configuration for each
  environment.
- `Dockerfile`, `docker-compose.yml`, and `deployments/metrics/`: complete local
  operator stack, health dependencies, traffic demo, alerts, and dashboard.
- `scripts/smoke-phase9.ps1`: native config/health/metrics/log smoke and Compose
  ingestion/dashboard prerequisites.
- `deployments/gcp/scripts/deploy.ps1`: upload the checked-in GCP config, run the
  exact release binary in `--check-config` mode before installation, install config
  as root-owned `0640`, and start systemd with `--config`.
- `deployments/gcp/scripts/metrics-tunnel.ps1`: fail-closed account/project guard and
  SSH local forward from operator `127.0.0.1:19100` to VM
  `127.0.0.1:9100`.
- `deployments/gcp/scripts/smoke-live.ps1` and `inspect-live.ps1`: traffic, metric
  delta, lag, log, config, filesystem, and restored-service assertions.
- `README.md`, `docs/operations.md`, `docs/gcp.md`: quickstart, metric catalog,
  PromQL, dashboards, config, tunnel, and incident runbooks.
- `proof/phase-9.md`: exact pushed/deployed SHA, CI run, local commands, Compose
  checks, tunnel queries, metric before/after evidence, journal evidence, systemd
  state, data/filesystem state, and Terraform drift result.

#### Implementation Sequence

1. Add the pinned YAML dependency, config schema, two-pass precedence logic,
   validation, check/print modes, and config tests before changing systemd or Compose.
2. Extract the structured logger without changing existing event meaning; run the
   current Phase 3 through Phase 8 tests and smokes to prove behavior is preserved.
3. Implement the metrics registry, histograms, renderer, and unit/golden tests with
   no broker integration.
4. Add read-only topic, partition, offset, and coordinator snapshot APIs with explicit
   lock-order tests and disappearing-series coverage.
5. Instrument connections, protocol operations, append/fetch completions, errors,
   overload, group events, retention, startup recovery, and shutdown. Counter updates
   occur at one defined success/failure boundary per operation.
6. Add the bounded admin HTTP parser, `/metrics`, finalized health status behavior,
   response headers, and concurrent integration tests.
7. Replace ad hoc command lines in Docker and GCP with checked-in validated config
   files; retain CLI overrides for smoke-only fault injection and backward
   compatibility.
8. Build the pinned Prometheus/Grafana Compose stack, provision the dashboard and
   alerts, and automate target/query validation in `smoke-phase9.ps1`.
9. Expand operations/GCP documentation and scripts, including the guarded SSH tunnel
   and a local Prometheus scrape target for that tunnel.
10. Run all local, CI, deployment, live metric/log, cleanup, and drift gates; only then
    write `proof/phase-9.md` and mark Phase 9 complete.

#### Local and CI Verification

- `--check-config` accepts every checked-in config and rejects fixtures for unknown,
  duplicate, mistyped, missing, overflowed, and invalid values before creating data
  files or binding ports.
- `--print-effective-config` proves defaults < YAML < CLI precedence and contains no
  token or environment value.
- GCC, Clang, and MSVC native builds compile the new dependency and observability
  code; the standard formatting, warning-as-error, unit, integration, and smoke gates
  remain green.
- Golden metrics pass `promtool check metrics`; Prometheus config and alerts pass
  `promtool check config`, `promtool check rules`, and `promtool test rules`.
- A deterministic integration test records a baseline scrape, produces and fetches
  records, forces an overload error, creates group lag, runs retention, then proves
  exact counter deltas and current gauges.
- The concurrency test drives append workers, long-poll fetch, group heartbeats,
  retention, topic lifecycle, and repeated `/metrics` scrapes together without data
  races, deadlocks, malformed output, or more than one scrape interval of blocking.
- Docker Compose reaches healthy state, one-shot demo services finish successfully,
  Prometheus reports `up{job="boltstream"} == 1`, documented PromQL returns samples,
  and Grafana provisioning exposes the BoltStream dashboard without manual edits.
- Existing Phase 2 through Phase 8 smoke scripts pass unchanged or with only their
  launch path migrated to the new config file.

#### Live GCP Verification

1. Commit and push the implementation; require all GitHub CI jobs to pass for the
   exact SHA and download that SHA's Linux release artifact.
2. Refresh the guarded operator `/32`, run Terraform plan, and require no unintended
   infrastructure changes. Phase 9 does not open port `9100` publicly.
3. Deploy the exact CI artifact plus checked-in GCP YAML. Prove remote
   `--check-config`, file ownership/mode, systemd `ExecStart --config`, `/version`
   SHA, `/health/live`, and `/health/ready`.
4. Start the guarded SSH tunnel and call `/metrics` through operator
   `127.0.0.1:19100`. Prove content type, Prometheus validation, build-info SHA,
   readiness, recovery, filesystem, and initial scrape counters.
5. Run authenticated live traffic and capture before/after samples proving produce
   and fetch request counts, records, protocol bytes, duration histogram counts,
   active connections, partition watermarks, segment bytes, and queue depth.
6. Create a durable consumer group with measurable lag, prove the group/member/lag
   series, consume to the high watermark, and prove the lag reaches zero.
7. Use a temporary validated config override to force one deterministic append
   overload, prove the error and rejection counters plus matching JSON journal event,
   then restore the checked-in GCP config and healthy service.
8. Run retention against a live disposable topic, prove deleted-segment/deleted-byte
   counter deltas and retained storage gauges, then delete the topic and prove its
   dynamic metric series disappear.
9. Point the local Prometheus/Grafana stack at the SSH tunnel, prove the live target
   is up, query the dashboard series, and inspect alert evaluation without installing
   observability daemons on the VM.
10. SSH-inspect systemd, JSON journal lines, effective config, release symlink, data
    files, disk capacity, and resource use. Remove disposable topics, stop the tunnel
    and local operator stack, restore the normal config, and require a final healthy
    service plus clean Terraform plan.

#### Acceptance

- Metrics expose traffic, latency, records/bytes, connections, storage, queue,
  long-poll, error, group, lag, retention, recovery, readiness, and filesystem state
  with stable documented names and bounded labels.
- A local operator can diagnose throughput, p95/p99 latency, append saturation, a
  slow consumer, disk growth, low free space, retention failures, and recovery from
  checked-in PromQL, dashboard panels, alerts, and JSON logs.
- File-backed configuration is real, strict, validated before side effects, safely
  overridden by CLI, deployed through systemd, and secret-free in source and output.
- The Compose demo is reproducible from a clean checkout and exercises the shipped
  broker, admin, producer, consumer, Prometheus, Grafana, dashboard, and rules.
- The live GCP broker exposes no new public operations port; the complete metrics and
  dashboard path is proven through a guarded SSH tunnel against the exact pushed and
  CI-tested commit.
- `proof/phase-9.md` records local, CI, Compose, GCP, metrics, logs, config, cleanup,
  and drift evidence. Phase 9 is not complete from local metrics screenshots or a
  successful `/health/ready` response alone.

### Phase 10: Benchmarking and Performance Engineering

#### Locked Boundary

- Preserve protocol version `4` and storage format version `2`. Phase 10 batches
  existing produce jobs internally; batch-produce framing and zstd negotiation remain
  Phase 11 work.
- Keep compatibility defaults at one I/O event-loop worker, two append workers, and
  one record per append batch. The three benchmark profiles select their runtime
  settings explicitly and never rewrite normal production configuration from noisy
  measurements.
- The 100k/400k/750k performance values remain aspirational developer-class goals,
  not release gates. Phase completion requires reproducible measurements, complete
  environment labels, zero request errors, and honest disclosure of missed goals.
- Durability is labeled `flush`: log and index C++ streams are flushed before produce
  responses. Phase 10 does not claim `fsync` durability.

#### Runtime Modes and Storage Batching

- Add strict YAML and CLI settings:
  - `runtime.io_workers` / `--io-workers`: `1..64`, default `1`, including the calling
    thread;
  - `storage.append_batch_records` / `--append-batch-records`: `1..1024`, default `1`;
  - `limits.append_workers=0`: inline append, valid only with one I/O worker and a
    batch size of one.
- Run the shared Asio `io_context` from the configured thread pool. Broker protocol
  sessions and the async client serialize their socket, timer, request, and response
  state through strands.
- Add `PartitionLog::append_batch`, retaining contiguous per-partition offsets and the
  existing record format. It opens each segment/log index once per batch chunk,
  flushes once, commits in-memory offsets only after both streams succeed, and rolls
  file lengths back on a write failure.
- Append workers drain at most the configured batch size from one partition without
  a linger timer. Each source request receives its own correlated response after the
  shared flush.
- Publish `boltstream_append_batches_total`, the
  `boltstream_append_batch_records` histogram, and runtime I/O/append worker gauges.
- Checked-in profiles are:
  - `single-threaded`: one I/O worker, inline append, batch size one;
  - `worker-event-loops`: two I/O workers, two append workers, batch size one;
  - `batched-writes`: two I/O workers, two append workers, batch size 32.

#### Benchmark Binaries and Result Contract

- `boltstream-bench run` drives authenticated `produce-throughput`,
  `produce-latency`, and `fetch-throughput` workloads over the public binary
  protocol. It supports explicit host/ports, profile metadata, deterministic topic,
  partition, client, warmup, duration/message, key/payload, repetition, timeout, and
  JSON/Markdown output controls. `--dry-run` remains non-publishing.
- Throughput counts only successful acknowledged records. Latency uses
  `steady_clock` from submission to correlated response and reports nearest-rank
  p50/p95/p99 plus the true maximum. Any request, transport, timeout, cleanup, or
  metrics failure invalidates the run.
- `boltstream-microbench` uses pinned Google Benchmark for protocol encode/decode,
  single append, batch append, segment roll, and read paths. It is installed in the
  same Release artifact but does not supply headline numbers.
- Versioned JSON records environment, actual CPU/OS/memory, compiler/build/SHA,
  protocol/storage versions, exact broker settings, workload, every repetition,
  throughput/MiB per second, latency, errors, and append-batch metric deltas.
- `scripts/render-benchmarks.ps1` accepts only one exact Release SHA, the complete
  three-profile/three-workload matrix, at least five error-free repetitions, and
  secret/IP-free inputs. It generates the canonical JSON, detailed Markdown, and the
  marked README table deterministically.

#### Canonical Workloads

- GCP produce throughput: four partitions, 16 producer connections, one outstanding
  request per connection, 256-byte values, 16-byte deterministic keys, 60 seconds of
  warmup, then 30 seconds measured.
- GCP acknowledged produce latency: the same loaded topology, 10,000 warmup records,
  then 100,000 measured records.
- GCP fetch throughput: preload 250,000 records, then use four partition-specific
  consumers to verify every record from the beginning. The untimed setup creates the
  topic through the authenticated broker, stops that isolated profile, writes the
  deterministic records with `boltstream-bench prepare-fetch` in storage batches of
  1,024, then restarts the same profile before the authenticated timed read. The JSON
  labels this `direct-batched-storage-setup`; setup time is excluded, but every fetched
  key, value, partition, offset, and total count is verified. Normal `run
  --workload fetch-throughput` retains its authenticated protocol preload.
- The initial campaign plan is five rounds with profile order rotated by round. The
  Phase 10 campaign was stopped by operator decision after two complete rounds for
  every profile plus a third complete round for single-threaded and batched-writes;
  the next worker-event-loops triplet was interrupted before local publication. The
  report retains every completed triplet, publishes medians, min/max, and coefficient
  of variation, and carries an explicit limited-sample/instability warning instead of
  spending another day on the shared-core VM. Future capacity studies may use
  `-Resume`; partial profiles are rerun and completed samples are never overwritten.

#### Verification and Live Proof

1. Cover config bounds/cross-fields, batch ordering/rolling/recovery, inline append,
   multi-threaded strands, metrics, percentiles, deterministic serialization,
   redaction, error exits, and dry runs with unit and integration tests.
2. Run GCC, Clang, MSVC, formatting, warnings-as-errors, existing phase smokes, all
   three Phase 10 profiles, Google Benchmark dry-run, report-regeneration checks, and
   a focused Linux ThreadSanitizer concurrency job. Shared CI runners never gate on
   numeric performance.
3. Run native Windows Release measurements as secondary evidence.
4. Push the implementation, require green CI for the exact SHA, and deploy that
   SHA's Linux Release artifact containing both benchmark binaries.
5. On the existing Ubuntu `e2-micro`, fail closed on the expected Google account,
   use loopback-only benchmark profiles and isolated `/var/lib/boltstream/phase10-*`
   data directories, then capture the complete rotated matrix. Open no new port.
6. Verify exact `/version`, readiness, systemd state, effective config, batch metrics,
   disk headroom, zero errors, topic/data cleanup, normal service restoration, and a
   clean Terraform plan.
7. Generate the GCP-primary README table, `docs/benchmarks.md`, canonical result JSON,
   and `proof/phase-10.md` with commands, CI run, artifact checksum, every workload,
   dispersion, metrics, cleanup, and final live state.

#### Acceptance

- All three modes work locally, in CI smoke, and against the exact live GCP artifact;
  the batched profile proves an average append batch above one while unbatched
  profiles remain exactly one.
- Produced/fetched counts match, every measured request succeeds, reports regenerate
  exactly, and no completed sample is cherry-picked. The operator-approved limited
  GCP campaign is sufficient for Phase 10 engineering evidence but is not represented
  as the originally planned five-round capacity study. Earlier phase behavior remains
  green.
- The normal GCP service is restored and healthy with no temporary override/data and
  no Terraform drift. Missing aspirational performance goals are documented as
  measured bottlenecks and do not block completion.

### Phase 11: Compression

#### Locked Boundary

- Upgrade the current protocol to version `5` and storage format to `3`, while the
  broker continues to accept protocol-v4 frames and mixed legacy version-1 records.
- Compression is client-side and per public produce batch. The broker validates with
  bounded decompression and persists the producer's exact `none` or `zstd` payload;
  it never recompresses. Replication, dictionaries, streaming compression, and new
  listeners remain Phase 12 or excluded work.
- Pin zstd `1.5.7` as a static FetchContent dependency with programs, tests, shared
  libraries, legacy APIs, and multithreaded compression disabled.

#### Protocol, Storage, and Operations

- Metadata v5 negotiates a required `none` capability and optional `zstd` capability
  per connection. A skipped negotiation permits only `none`; changing protocol
  versions on an established connection is rejected.
- Add batch-produce frames `38/39`, a canonical length-prefixed key/value record set,
  explicit partition, codec, record count, logical/encoded byte sizes, and a response
  containing the assigned contiguous offset range and stored byte count.
- Add strict batch limits of 1,024 records and 1 MiB uncompressed by default. Reject
  unsupported codecs, corrupt frames, count/size mismatches, empty values, and zstd
  expansion beyond the configured bound before storage mutation.
- Storage batch version `2` carries codec, base offset, shared append timestamp,
  record count, logical/encoded sizes, exact payload, and an outer CRC. Every logical
  offset is indexed to the physical batch. Recovery supports legacy and batch entries
  in one segment and truncates a corrupt trailing batch atomically.
- Aligned v5 zstd fetches pass the stored batch through unchanged when negotiated and
  within fetch/frame limits. V4, `none`, mid-batch, and limited fetches decode to the
  existing record view without gaps or duplicates.
- Producer/consumer CLIs expose `--compression`; producer additionally exposes
  `--batch-records` and `--zstd-level`. Logtool exposes `inspect-batch`. Prometheus
  publishes fixed-cardinality codec batch/byte, decode-failure, and pass-through
  counters.

#### Verification and Live Proof

1. Cover codec bounds, protocol v4/v5 shapes, negotiation, mixed-format recovery,
   corruption, mid-batch fallback, CLI round trips, metrics, and exact offsets locally.
2. Run Linux Debug/Release, Windows MSVC, formatting, warnings-as-errors, existing
   phase smokes, the Phase 11 smoke, and focused ThreadSanitizer coverage in CI.
3. Compare one functional `none`/`zstd` smoke pair on the batched-writes settings with
   32 records, level 3, and the deterministic compressible payload. Publish throughput,
   batch acknowledgement latency, fetch throughput, and physical log bytes, explicitly
   without a capacity or statistical-performance claim.
4. Deploy the exact pushed and CI-tested Release artifact to the existing GCP VM. Use
   isolated loopback listeners and `/var/lib/boltstream/phase11-*`, inspect stored zstd
   batches and metrics, restore the normal service, remove temporary state, and prove
   readiness, version, disk headroom, and a clean Terraform plan in `proof/phase-11.md`.

#### Acceptance

- V4, v5-none, and v5-zstd clients interoperate; aligned zstd fetch proves pass-through
  and legacy/mid-batch fetch proves safe decode fallback.
- The deterministic zstd smoke has zero errors, exact produced/fetched counts, and
  fewer partition-log bytes than `none`; reports do not overstate the single sample.
- Local, CI, packaged-artifact, isolated live GCP, cleanup, and drift evidence are all
  durable. Existing normal data remains readable; rollback after normal data receives
  format-3 batches requires the Phase 11 binary or a pre-enable snapshot.

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
- [Prometheus scrape content-negotiation documentation](https://prometheus.io/docs/instrumenting/content_negotiation/) keeps Prometheus text `0.0.4` as the required fallback when no other supported format is negotiated; Phase 9 deliberately implements that stable compatibility baseline.
- [Prometheus exporter guidance](https://prometheus.io/docs/instrumenting/writing_exporters/) requires base units, `_total` counters, cumulative histogram buckets, minimal labels, and scrape-time timestamps supplied by Prometheus rather than the target; the Phase 9 metric catalog follows those rules.
- The current pinned source releases checked during Phase 9 planning are [yaml-cpp `0.9.0`](https://github.com/jbeder/yaml-cpp/releases/tag/yaml-cpp-0.9.0), [Prometheus `3.13.0` LTS](https://github.com/prometheus/prometheus/releases/tag/v3.13.0), and [Grafana `13.1.0`](https://github.com/grafana/grafana/releases/tag/v13.1.0). Re-check their image digests immediately before implementation and record the chosen immutable digests in the repository.

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
