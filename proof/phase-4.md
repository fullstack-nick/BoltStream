# Phase 4 Proof - Broker Produce and Fetch

Date: 2026-07-08

Status: local implementation and Linux parity verified. Push, CI artifact, GCP deploy,
live broker proof, SSH inspection, and Terraform drift proof are still required before
Phase 4 is considered fully complete.

## Local Verification

Windows GCC build:

```text
.\scripts\build.ps1 -Preset windows-gcc-debug
...
Linking CXX executable boltstream_tests.exe
```

Windows GCC tests:

```text
.\scripts\test.ps1 -Preset windows-gcc-debug
100% tests passed, 0 tests failed out of 32
```

Phase 4 CLI smoke:

```text
.\scripts\smoke-phase4.ps1 -Preset windows-gcc-debug
Phase 4 broker produce/fetch smoke passed on 127.0.0.1:49039 with data dir C:\Users\napet\AppData\Local\Temp\boltstream-phase4-smoke-54432e9f-dee2-4322-84c9-25a6b75e573b.
```

Compatibility smoke entrypoint:

```text
.\scripts\smoke-phase2.ps1 -Preset windows-gcc-debug
Phase 4 broker produce/fetch smoke passed on 127.0.0.1:32074 with data dir C:\Users\napet\AppData\Local\Temp\boltstream-phase4-smoke-35678876-fd4f-4e31-8d5e-2444be52558a.
```

Additional Windows compiler builds:

```text
.\scripts\build.ps1 -Preset windows-msvc-debug
Linking CXX executable boltstream_tests.exe

.\scripts\build.ps1 -Preset windows-clang-debug
Linking CXX executable boltstream_tests.exe
```

Docker Linux parity:

```text
docker build --target builder -t boltstream-builder .
```

The Docker build command hit the local command timeout after the image was created.
The resulting `boltstream-builder:latest` image contained the completed Linux debug
test and release build from the Dockerfile.

Explicit Linux test readback from the builder image:

```text
docker run --rm boltstream-builder ctest --preset test-linux-gcc-debug
100% tests passed, 0 tests failed out of 32
```

Windows package:

```text
.\scripts\package.ps1 -Preset windows-gcc-debug
Created C:\D_DRIVE\Nikita\JS\BoltStream\artifacts\boltstream-windows-x86_64-def9f03bc7b2.zip
```

## Behavior Verified

- Broker `ProduceRequest` appends to the durable single-partition log and returns
  topic, partition, offset, next offset, and encoded byte size.
- Broker `FetchRequest` reads by `beginning`, `latest`, and explicit offset.
- Metadata lists open or created topics with partition `0` and next offset.
- Existing topic logs reopen after broker restart.
- Invalid topic names return structured malformed-payload errors.
- `BOLTSTREAM_BROKER_TOKEN` enforces auth for metadata, produce, and fetch while
  health remains unauthenticated.
- Concurrent producers receive unique monotonic offsets.
- Correlation ids are preserved across multiple in-flight client requests.

## Pending Live Gate

- Push the implementation commit to `origin/main`.
- Wait for GitHub Actions CI to pass and produce the Linux release artifact.
- Deploy the exact CI artifact to GCP with `deployments/gcp/scripts/deploy.ps1`.
- Run authenticated live producer/consumer smoke through TCP `9000`.
- Inspect systemd status, journal logs, release symlink, and topic files over SSH.
- Run Terraform drift check.
