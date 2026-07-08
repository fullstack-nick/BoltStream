# Phase 8 Proof - Retention and Topic Lifecycle

Date: 2026-07-08

Status: complete for runtime commit `bfa10c9c4c68`.

## Scope

Phase 8 implements broker-backed retention and topic lifecycle controls. The
feature is intentionally broker-level policy, not per-topic retention editing.

Implemented behavior:

- Protocol version `4`.
- New lifecycle/admin frames for topic list, topic describe, topic delete,
  retention run, group describe, and group offset reset.
- New `offset_out_of_range` and `group_active` protocol errors.
- Broker CLI options for segment size, segment max age, retention max age,
  retention max bytes, and retention check interval.
- Size and age segment rolling.
- Retention deletion of inactive complete segments only.
- Low-watermark fetch semantics through `earliest_offset`.
- Topic delete protection for active coordinated groups.
- Durable cleanup of inactive group offsets for deleted topics.
- Inactive group offset reset with bounds checking.

## Local Verification

Formatting and diff hygiene:

```text
.\scripts\format.ps1 -Fix
.\scripts\format.ps1
git diff --check
```

All commands passed. `git diff --check` emitted only the existing Git line-ending
normalization warning for `deployments/gcp/scripts/smoke-live.ps1`.

Windows GCC build:

```text
cmake --build build\windows-gcc-debug --target boltstream_tests boltstream-admin boltstream-server boltstream-producer boltstream-consumer --config Debug
```

The target build passed after CMake regenerated the build files.

Windows GCC tests:

```text
.\build\windows-gcc-debug\boltstream_tests.exe
65 tests from 6 test suites passed.

.\scripts\test.ps1 -Preset windows-gcc-debug
100% tests passed, 0 tests failed out of 65
```

Phase 8 local smoke:

```text
.\scripts\smoke-phase8.ps1 -Preset windows-gcc-debug
Phase 8 retention and topic lifecycle smoke passed.
```

The smoke starts a local authenticated broker with tiny segment and retention
settings, creates a topic, rolls multiple inactive segments, ages retained
segments, runs admin retention, proves `offset_out_of_range` for a retained-away
offset, fetches from `beginning` at the new low watermark, commits and resets an
inactive group offset, deletes the topic, and verifies the topic is no longer
discoverable.

## GitHub CI

Implementation commit:

```text
bfa10c9 Implement Phase 8 retention lifecycle
```

CI run `28975140387` passed for
`bfa10c9c4c68e16cc450f0732762a827ff2ee928`:

```text
Linux Debug: success
Linux Release: success
Windows MSVC smoke: success
```

The Linux release artifact downloaded from that run was:

```text
artifacts/ci-phase8-bfa10c9c4c68/boltstream-linux-x86_64-bfa10c9c4c68e16cc450f0732762a827ff2ee928/boltstream-linux-x86_64-bfa10c9c4c68.tar.gz
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
  -Artifact .\artifacts\ci-phase8-bfa10c9c4c68\boltstream-linux-x86_64-bfa10c9c4c68e16cc450f0732762a827ff2ee928\boltstream-linux-x86_64-bfa10c9c4c68.tar.gz `
  -GitSha bfa10c9c4c68
```

Deploy result:

```text
Active: active (running) since Wed 2026-07-08 21:00:40 UTC
/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
{"service":"boltstream","version":"0.1.0","git_sha":"bfa10c9c4c68","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"4","storage_format_version":"2","startup_time_utc":"2026-07-08T21:00:40Z"}
```

Authenticated live smoke:

```text
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha bfa10c9c4c68 -BuildDir build\windows-gcc-debug
TCP broker port 9000 reachable.
Live broker produce/fetch succeeded for topic live-phase7-basic-20260708210601.
Live broker group commit/resume succeeded for topic live-phase7-basic-20260708210601.
Live coordinated group split and timeout takeover succeeded for topic live-phase7-20260708210604.
Live Phase 8 retention, group reset, and topic delete succeeded for topic live-phase8-20260708210657.
```

The Phase 8 live smoke temporarily restarted the service with a tiny
`--segment-bytes 96` setting and disabled only the periodic retention timer. It
kept the default seven-day retention policy, produced three segments, aged the
inactive segments to eight days old over SSH, ran `retention run --topic`,
proved `offset_out_of_range` below the retained low watermark, fetched from
`beginning`, committed and reset an inactive group offset, deleted the topic,
verified the topic path was gone, and restored the normal systemd command.

Live health and version output after cleanup:

```text
live:
{"service":"boltstream","status":"live","git_sha":"bfa10c9c4c68","detail":"ready"}
ready:
{"service":"boltstream","status":"ready","git_sha":"bfa10c9c4c68","detail":"ready"}
version:
{"service":"boltstream","version":"0.1.0","git_sha":"bfa10c9c4c68","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"4","storage_format_version":"2","startup_time_utc":"2026-07-08T21:07:41Z"}
```

## SSH Inspection

Before live retention:

```text
phase8 before retention:
00000000000000000000.index 20 bytes
00000000000000000000.log 81 bytes
00000000000000000001.index 20 bytes
00000000000000000001.log 81 bytes
00000000000000000002.index 20 bytes
00000000000000000002.log 81 bytes
```

After topic delete:

```text
phase8 after delete topic path:
deleted
```

Final service state after cleanup:

```text
Active: active (running) since Wed 2026-07-08 21:07:41 UTC
/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
{"service":"boltstream","version":"0.1.0","git_sha":"bfa10c9c4c68","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"4","storage_format_version":"2","startup_time_utc":"2026-07-08T21:07:41Z"}
```

Disk and deleted-topic inspection:

```text
/dev/sdb         20G  780K   19G   1% /var/lib/boltstream
phase8_topic_deleted
```

Focused journal proof, with client endpoint fields intentionally omitted:

```json
{"timestamp":"2026-07-08T21:07:13Z","level":"info","event":"retention_applied","waiter_count":0,"message":"topics_scanned=1 partitions_scanned=1 segments_deleted=2 bytes_deleted=162"}
{"timestamp":"2026-07-08T21:07:14Z","level":"warn","event":"protocol_error","correlation_id":2,"frame_type":"fetch_request","error_code":"offset_out_of_range","retryable":false,"waiter_count":0,"request_duration_ms":0,"message":"fetch offset is below partition earliest offset"}
{"timestamp":"2026-07-08T21:07:17Z","level":"info","event":"topic_deleted","waiter_count":0,"message":"topic=live-phase8-20260708210657 partitions_deleted=1 segments_deleted=1 bytes_deleted=81 offsets_removed=1"}
```

## Behavior Verified Locally

- `/version` build metadata reports protocol version `4` in tests.
- Protocol tests cover every new Phase 8 payload, frame name, error name, and
  malformed input path.
- `PartitionLog` rolls by segment age and segment size.
- Retention deletes only inactive segments and preserves recovery from the first
  remaining segment.
- Fetch below `earliest_offset` returns `offset_out_of_range`.
- Fetch from `beginning` starts at `earliest_offset` after retention.
- Offset commits and group resets are bounded to `[earliest_offset, next_offset]`.
- Offset store topic cleanup persists after reload.
- Admin topic list, describe, delete, retention run, group describe, and group
  reset are covered over TCP with authentication.
- Topic delete and group reset reject active coordinated groups with
  `group_active`.

## Behavior Verified Live

- `/version` reports protocol version `4` for deployed commit `bfa10c9c4c68`.
- The live broker retained away two inactive segments and kept the active
  segment recoverable.
- Fetching offset `0` after retention returned `offset_out_of_range`.
- Fetching from `beginning` resumed at retained low watermark offset `2`.
- Group describe observed committed offset `3` after the retained record was
  consumed and committed.
- Group reset to `beginning` wrote durable offset `2` for the inactive group.
- Topic delete removed the topic directory and one remaining active segment.
- The normal systemd command was restored after proof, with the service active.
