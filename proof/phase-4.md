# Phase 4 Proof - Broker Produce and Fetch

Date: 2026-07-08

Status: complete for runtime commit `fa4becc507df`.

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
.\scripts\package.ps1 -Preset windows-gcc-debug -GitSha fa4becc507df
Created C:\D_DRIVE\Nikita\JS\BoltStream\artifacts\boltstream-windows-x86_64-fa4becc507df.zip
```

Authenticated local CLI smoke after fixing the auth continuation:

```text
.\scripts\smoke-phase4.ps1 -Preset windows-gcc-debug
Phase 4 broker produce/fetch smoke passed on 127.0.0.1:24430 with data dir C:\Users\napet\AppData\Local\Temp\boltstream-phase4-smoke-9ebf5187-016a-40a1-9eb9-6ab4b5cb5512.
```

Follow-up compiler checks after the auth-continuation fix:

```text
.\scripts\build.ps1 -Preset windows-msvc-debug
Linking CXX executable boltstream-consumer.exe

.\scripts\build.ps1 -Preset windows-clang-debug
Linking CXX executable boltstream-consumer.exe
```

## GitHub CI

Implementation commit:

```text
472cfad707da4b34a49f168d1cf5cdf36a251e51 Implement broker produce and fetch
```

Follow-up auth-continuation fix:

```text
fa4becc507df3f8aa112f9add443d22d3d132d53 Fix CLI auth continuation
```

CI run `28933200206` passed for `fa4becc507df3f8aa112f9add443d22d3d132d53`:

```text
Linux Release: success
Linux Debug: success
Windows MSVC smoke: success
```

The release artifact downloaded from that run was:

```text
artifacts/boltstream-linux-x86_64-fa4becc507df.tar.gz
```

## GCP Deploy And Live Proof

The broker token Secret Manager resource existed without a payload before Phase 4
deploy. A generated broker token was added as version `2`; the accidental initial
version `1` was destroyed and no token value was committed.

Deploy command:

```text
.\deployments\gcp\scripts\deploy.ps1 `
  -Artifact .\artifacts\boltstream-linux-x86_64-fa4becc507df.tar.gz `
  -GitSha fa4becc507df
```

Deploy result:

```text
Active: active (running)
/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
{"service":"boltstream","version":"0.1.0","git_sha":"fa4becc507df","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"1","storage_format_version":"1","startup_time_utc":"2026-07-08T09:47:17Z"}
```

Authenticated live smoke:

```text
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha fa4becc507df -BuildDir build\windows-gcc-debug
TCP broker port 9000 reachable.
Live broker produce/fetch succeeded for topic live-phase4-20260708094729.
ready:
{"service":"boltstream","status":"ready","git_sha":"fa4becc507df","detail":"ready"}
version:
{"service":"boltstream","version":"0.1.0","git_sha":"fa4becc507df","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"1","storage_format_version":"1","startup_time_utc":"2026-07-08T09:47:17Z"}
```

Live topic files created by broker produce:

```text
/var/lib/boltstream/topics/live-phase4-20260708094729/partition-000000/00000000000000000000.index
/var/lib/boltstream/topics/live-phase4-20260708094729/partition-000000/00000000000000000000.log
```

Journal proof after live calls:

```text
protocol request correlation_id=1 type=auth_request payload_bytes=47
protocol request correlation_id=2 type=produce_request payload_bytes=57
protocol request correlation_id=1 type=auth_request payload_bytes=47
protocol request correlation_id=2 type=fetch_request payload_bytes=43
```

SSH inspection:

```text
.\deployments\gcp\scripts\inspect-live.ps1
Active: active (running)
data dir writable by boltstream
/opt/boltstream/releases/fa4becc507df
```

Terraform drift:

```text
terraform plan -detailed-exitcode
No changes. Your infrastructure matches the configuration.
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

## Completion Decision

Phase 4 is complete for runtime commit `fa4becc507df`: the broker produces and
fetches durable records through the TCP protocol locally and live on GCP; auth is
required on the deployed broker; CI passed; the exact CI Linux artifact was
deployed; live producer/consumer calls succeeded; SSH inspection proved runtime,
logs, and topic files; and Terraform reported no drift.
