# Phase 8 Proof - Retention and Topic Lifecycle

Date: 2026-07-08

Status: local implementation verified. Commit, CI, deploy, and live proof are
pending at the time this file was created.

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

Pending.

## GCP Deploy And Live Proof

Pending.

## SSH Inspection

Pending.

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
