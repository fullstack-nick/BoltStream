# Phase 6 Proof - Backpressure and Robustness

Date: 2026-07-08

Status: complete for runtime commit `74d838e77b32`.

## Local Verification

Formatting and diff hygiene:

```text
.\scripts\format.ps1
git diff --check
```

Both commands passed.

Windows GCC build and tests:

```text
.\scripts\build.ps1 -Preset windows-gcc-debug
.\scripts\test.ps1 -Preset windows-gcc-debug
100% tests passed, 0 tests failed out of 46
```

Windows MSVC build and tests:

```text
.\scripts\build.ps1 -Preset windows-msvc-debug
.\scripts\test.ps1 -Preset windows-msvc-debug
100% tests passed, 0 tests failed out of 46
```

Windows Clang build and tests:

```text
.\scripts\build.ps1 -Preset windows-clang-debug
.\scripts\test.ps1 -Preset windows-clang-debug
100% tests passed, 0 tests failed out of 46
```

Phase 6 CLI smoke:

```text
.\scripts\smoke-phase6.ps1 -Preset windows-gcc-debug
Phase 6 backpressure/robustness smoke passed.

.\scripts\smoke-phase6.ps1 -Preset windows-msvc-debug
Phase 6 backpressure/robustness smoke passed.
```

The Phase 6 smoke covers normal produce/fetch/group/long-poll behavior,
deterministic append overload, deterministic long-poll overload, oversized-frame
abuse, and structured log checks.

## GitHub CI

Implementation commits:

```text
beed4a8 Implement Phase 6 backpressure robustness
74d838e Fix Phase 6 smoke exit code
```

CI run `28954383016` passed for
`74d838e77b3211d944a6ce4380a4d0395d6c286b`:

```text
Linux Debug: success
Linux Release: success
Windows MSVC smoke: success
```

The Windows CI job includes `scripts/smoke-phase6.ps1`.

The release artifact downloaded from that run was:

```text
artifacts/ci-phase6/boltstream-linux-x86_64-74d838e77b3211d944a6ce4380a4d0395d6c286b/boltstream-linux-x86_64-74d838e77b32.tar.gz
```

## GCP Deploy And Live Proof

The deploy target was the existing Terraform-managed VM:

```text
project: boltstream-r7m5o9ld
zone: us-central1-a
instance: boltstream-vm
service: boltstream.service
```

Deploy command:

```text
.\deployments\gcp\scripts\deploy.ps1 `
  -Artifact .\artifacts\ci-phase6\boltstream-linux-x86_64-74d838e77b3211d944a6ce4380a4d0395d6c286b\boltstream-linux-x86_64-74d838e77b32.tar.gz `
  -GitSha 74d838e77b32
```

Deploy result:

```text
Active: active (running)
/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
{"service":"boltstream","version":"0.1.0","git_sha":"74d838e77b32","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"2","storage_format_version":"2","startup_time_utc":"2026-07-08T15:27:40Z"}
```

Authenticated live smoke:

```text
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha 74d838e77b32 -BuildDir build\windows-gcc-debug
TCP broker port 9000 reachable.
Live broker produce/fetch succeeded for topic live-phase5-20260708152751.
Live broker group commit/resume succeeded for topic live-phase5-20260708152751.
```

Live health and version output:

```text
live:
{"service":"boltstream","status":"live","git_sha":"74d838e77b32","detail":"ready"}
ready:
{"service":"boltstream","status":"ready","git_sha":"74d838e77b32","detail":"ready"}
version:
{"service":"boltstream","version":"0.1.0","git_sha":"74d838e77b32","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"2","storage_format_version":"2","startup_time_utc":"2026-07-08T15:27:40Z"}
```

Journal proof after authenticated live calls, with public client endpoints
redacted:

```json
{"timestamp":"2026-07-08T15:27:40Z","level":"info","event":"storage_recovery","waiter_count":0,"message":"topics=6 partitions=14 segments=15 indexes_rebuilt=15 records=7 bytes_truncated=0"}
{"timestamp":"2026-07-08T15:27:40Z","level":"info","event":"server_listening","waiter_count":0,"message":"broker=0.0.0.0:9000 admin=127.0.0.1:9100 data=/var/lib/boltstream"}
{"timestamp":"2026-07-08T15:27:55Z","level":"info","event":"protocol_request","remote":"<redacted>","correlation_id":2,"frame_type":"create_topic_request","payload_bytes":32,"waiter_count":0}
{"timestamp":"2026-07-08T15:27:55Z","level":"info","event":"protocol_response","remote":"<redacted>","correlation_id":2,"frame_type":"create_topic_response","payload_bytes":43,"waiter_count":0,"request_duration_ms":0}
{"timestamp":"2026-07-08T15:27:55Z","level":"info","event":"protocol_request","remote":"<redacted>","correlation_id":2,"frame_type":"produce_request","payload_bytes":57,"waiter_count":0}
{"timestamp":"2026-07-08T15:27:55Z","level":"info","event":"protocol_response","remote":"<redacted>","correlation_id":2,"frame_type":"produce_response","payload_bytes":52,"waiter_count":0,"request_duration_ms":0}
{"timestamp":"2026-07-08T15:27:57Z","level":"info","event":"protocol_request","remote":"<redacted>","correlation_id":3,"frame_type":"offset_commit_request","payload_bytes":54,"waiter_count":0}
{"timestamp":"2026-07-08T15:27:57Z","level":"info","event":"protocol_response","remote":"<redacted>","correlation_id":3,"frame_type":"offset_commit_response","payload_bytes":54,"waiter_count":0,"request_duration_ms":0}
```

## Constrained Phase 6 Overload Proof

A temporary loopback broker was launched on the VM with the same release binary
and deterministic overload limits:

```text
/opt/boltstream/current/bin/boltstream-server \
  --listen 127.0.0.1:19000 \
  --admin-listen 127.0.0.1:19100 \
  --data /tmp/boltstream-phase6-proof-data \
  --max-append-queue-depth 0 \
  --max-long-poll-waiters 0
```

Loopback `/version`:

```json
{"service":"boltstream","version":"0.1.0","git_sha":"74d838e77b32","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"2","storage_format_version":"2","startup_time_utc":"2026-07-08T15:29:31Z"}
```

Constrained overload output:

```text
create={"status":"created","topic":"phase6proof","partitions":1,"correlation_id":1}
produce_status=5
produce={"status":"overloaded","error_code":"overloaded","retryable":true,"message":"append queue is full","correlation_id":1}
wait_status=5
wait={"status":"overloaded","error_code":"overloaded","retryable":true,"message":"long-poll waiter limit reached","correlation_id":1}
fetch={"status":"ok","topic":"phase6proof","partition":0,"from":0,"count":0,"next_offset":0,"records":[],"correlation_id":1}
```

Constrained structured log snippets:

```json
{"timestamp":"2026-07-08T15:29:31Z","level":"warn","event":"append_overloaded","error_code":"overloaded","retryable":true,"append_queue_depth":0,"waiter_count":0,"message":"append queue is full"}
{"timestamp":"2026-07-08T15:29:31Z","level":"warn","event":"protocol_error","remote":"127.0.0.1:44666","correlation_id":1,"frame_type":"produce_request","error_code":"overloaded","retryable":true,"waiter_count":0,"request_duration_ms":2,"message":"append queue is full"}
{"timestamp":"2026-07-08T15:29:31Z","level":"warn","event":"long_poll_overloaded","remote":"127.0.0.1:44674","correlation_id":1,"frame_type":"fetch_request","error_code":"overloaded","retryable":true,"waiter_count":0,"message":"long-poll waiter limit reached"}
{"timestamp":"2026-07-08T15:29:31Z","level":"warn","event":"protocol_error","remote":"127.0.0.1:44674","correlation_id":1,"frame_type":"fetch_request","error_code":"overloaded","retryable":true,"waiter_count":0,"request_duration_ms":0,"message":"long-poll waiter limit reached"}
```

## SSH Inspection

Live service inspection:

```text
.\deployments\gcp\scripts\inspect-live.ps1
Active: active (running) since Wed 2026-07-08 15:27:40 UTC
Main PID: 10449 (boltstream-serv)
/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
```

Admin `/version` during inspection:

```json
{"service":"boltstream","version":"0.1.0","git_sha":"74d838e77b32","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"2","storage_format_version":"2","startup_time_utc":"2026-07-08T15:27:40Z"}
```

Data directory and release inspection:

```text
/dev/sdb         20G  236K   19G   1% /var/lib/boltstream
data dir writable by boltstream
/var/lib/boltstream/topics/live-phase5-20260708152751/manifest.json
/var/lib/boltstream/topics/live-phase5-20260708152751/partition-000000/00000000000000000000.log
/var/lib/boltstream/topics/live-phase5-20260708152751/partition-000001/00000000000000000000.log
/var/lib/boltstream/topics/live-phase5-20260708152751/partition-000002/00000000000000000000.log
/var/lib/boltstream/consumer_offsets/livephase5/offsets.log
live-phase5-20260708152751	1	1	4265450295
/opt/boltstream/releases/74d838e77b32
```

Terraform drift after proof:

```text
terraform plan -detailed-exitcode -no-color
No changes. Your infrastructure matches the configuration.
```

## Behavior Verified

- Protocol and storage versions remain `2`.
- `overloaded` is protocol error code `16` and is retryable.
- Producer, consumer, and admin CLI error JSON includes `retryable`; retryable
  overload exits with code `5`.
- Append admission is bounded per partition and `--max-append-queue-depth 0`
  rejects appends before storage mutation.
- Appends run through a bounded worker pool while preserving per-partition order.
- Long-poll waiter state is globally bounded and `--max-long-poll-waiters 0`
  rejects waiting fetches while immediate fetch still works.
- Oversized frame handling closes the connection after header validation without
  accepting the payload.
- Fetch response truncation returns `next_offset` as the consumer resume offset.
- Structured broker logs include event, correlation id, frame type, payload
  bytes, retryable error data, waiter count, queue depth where applicable, and
  request duration without logging broker tokens.
- The broker connection cap is covered by integration tests.
- CI now runs the Phase 6 smoke in the Windows MSVC job.

## Completion Decision

Phase 6 is complete for runtime commit `74d838e77b32`: the implementation is
pushed, CI is green, the exact CI Linux release artifact is deployed to GCP,
normal authenticated produce/fetch/group behavior works against the public
broker, deterministic append and long-poll overload behavior is live proven on
the VM with the same release binary, SSH inspection proves systemd, journal,
data, offsets, release contents, and runtime version, and Terraform reports no
drift after proof.
