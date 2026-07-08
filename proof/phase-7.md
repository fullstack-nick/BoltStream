# Phase 7 Proof - Coordinated Consumer Groups

Date: 2026-07-08

Status: complete for runtime commit `4b68b9a3867c`.

## Scope

Phase 7 implements broker-managed coordinated consumer groups for one topic per
`(group, topic)` pair. Multi-topic subscriptions are intentionally not part of
this phase and were not added to the committed roadmap.

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
100% tests passed, 0 tests failed out of 56
```

Windows MSVC build and tests:

```text
.\scripts\build.ps1 -Preset windows-msvc-debug
.\scripts\test.ps1 -Preset windows-msvc-debug
100% tests passed, 0 tests failed out of 56
```

Windows Clang build and tests:

```text
.\scripts\build.ps1 -Preset windows-clang-debug
.\scripts\test.ps1 -Preset windows-clang-debug
100% tests passed, 0 tests failed out of 56
```

Phase 7 CLI smoke:

```text
.\scripts\smoke-phase7.ps1 -Preset windows-gcc-debug
Phase 7 coordinated consumer group smoke passed.

.\scripts\smoke-phase7.ps1 -Preset windows-msvc-debug
Phase 7 coordinated consumer group smoke passed.
```

After the final smoke-script stabilization, formatting, `git diff --check`, and
the GCC/MSVC Phase 7 smokes were rerun successfully.

## GitHub CI

Implementation commits:

```text
308f303 Implement Phase 7 coordinated consumer groups
c30f050 Stabilize Phase 7 smoke timeouts
b6e74c5 Handle oversized frame close race in test
2dd582b Report protocol version 3 in build info
101a8eb Retry coordinated sync rebalances
4b68b9a Stabilize Phase 7 takeover smoke assertion
```

CI run `28971494292` passed for
`4b68b9a3867cf4ab5540cabf6894f0be39befaf8`:

```text
Linux Debug: success
Linux Release: success
Windows MSVC smoke: success
```

The Windows CI job includes `scripts/smoke-phase7.ps1`.

The release artifact downloaded from that run was:

```text
artifacts/ci-phase7-4b68b9a3867c/boltstream-linux-x86_64-4b68b9a3867c.tar.gz
```

## GCP Deploy And Live Proof

The deploy target was the existing Terraform-managed VM:

```text
project: boltstream-r7m5o9ld
zone: us-central1-a
instance: boltstream-vm
service: boltstream.service
```

The operator firewall source range was refreshed through ignored local
Terraform variables before deployment. Terraform planned and applied only the
SSH and broker firewall source-range updates.

Deploy command:

```text
.\deployments\gcp\scripts\deploy.ps1 `
  -Artifact .\artifacts\ci-phase7-4b68b9a3867c\boltstream-linux-x86_64-4b68b9a3867c.tar.gz `
  -GitSha 4b68b9a3867c
```

Deploy result:

```text
Active: active (running) since Wed 2026-07-08 20:03:51 UTC
/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
{"service":"boltstream","version":"0.1.0","git_sha":"4b68b9a3867c","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"3","storage_format_version":"2","startup_time_utc":"2026-07-08T20:03:51Z"}
```

Authenticated live smoke:

```text
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha 4b68b9a3867c -BuildDir build\windows-gcc-debug
TCP broker port 9000 reachable.
Live broker produce/fetch succeeded for topic live-phase7-basic-20260708200406.
Live broker group commit/resume succeeded for topic live-phase7-basic-20260708200406.
Live coordinated group split and timeout takeover succeeded for topic live-phase7-20260708200409.
```

Live health and version output:

```text
live:
{"service":"boltstream","status":"live","git_sha":"4b68b9a3867c","detail":"ready"}
ready:
{"service":"boltstream","status":"ready","git_sha":"4b68b9a3867c","detail":"ready"}
version:
{"service":"boltstream","version":"0.1.0","git_sha":"4b68b9a3867c","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"3","storage_format_version":"2","startup_time_utc":"2026-07-08T20:03:51Z"}
```

Focused group-coordinator journal proof:

```json
{"timestamp":"2026-07-08T20:04:11Z","level":"info","event":"group_member_joined","assignment":"0,1,2,3","group":"livephase7","member_id":"member-000000000001","topic":"live-phase7-20260708200409","generation_id":1}
{"timestamp":"2026-07-08T20:04:12Z","level":"info","event":"group_member_joined","assignment":"2,3","group":"livephase7","member_id":"member-000000000002","topic":"live-phase7-20260708200409","generation_id":2}
{"timestamp":"2026-07-08T20:04:15Z","level":"info","event":"group_heartbeat","assignment":"0,1","group":"livephase7","member_id":"member-000000000001","topic":"live-phase7-20260708200409","generation_id":2}
{"timestamp":"2026-07-08T20:04:17Z","level":"info","event":"group_offset_committed","assignment":"2,3","group":"livephase7","member_id":"member-000000000002","topic":"live-phase7-20260708200409","generation_id":2,"next_offset":1,"partition":2}
{"timestamp":"2026-07-08T20:04:23Z","level":"info","event":"group_member_expired","assignment":"","group":"livephase7","member_id":"member-000000000002","topic":"live-phase7-20260708200409","generation_id":3}
{"timestamp":"2026-07-08T20:04:27Z","level":"info","event":"group_offset_committed","assignment":"0,1,2,3","group":"livephase7","member_id":"member-000000000001","topic":"live-phase7-20260708200409","generation_id":3,"next_offset":1,"partition":3}
{"timestamp":"2026-07-08T20:04:55Z","level":"info","event":"group_member_left","assignment":"0,1,2,3","group":"livephase7","member_id":"member-000000000001","topic":"live-phase7-20260708200409","generation_id":4}
```

Protocol request logs include client endpoints; those are intentionally omitted
from this public proof.

## SSH Inspection

Live service inspection:

```text
.\deployments\gcp\scripts\inspect-live.ps1
Active: active (running) since Wed 2026-07-08 20:03:51 UTC
Main PID: 13183 (boltstream-serv)
/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
```

Admin `/version` during inspection:

```json
{"service":"boltstream","version":"0.1.0","git_sha":"4b68b9a3867c","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"3","storage_format_version":"2","startup_time_utc":"2026-07-08T20:03:51Z"}
```

Data directory and release inspection:

```text
/dev/sdb         20G  472K   19G   1% /var/lib/boltstream
data dir writable by boltstream
/var/lib/boltstream/topics/live-phase7-20260708200409/manifest.json
/var/lib/boltstream/topics/live-phase7-20260708200409/partition-000000/00000000000000000000.log
/var/lib/boltstream/topics/live-phase7-20260708200409/partition-000001/00000000000000000000.log
/var/lib/boltstream/topics/live-phase7-20260708200409/partition-000002/00000000000000000000.log
/var/lib/boltstream/topics/live-phase7-20260708200409/partition-000003/00000000000000000000.log
/opt/boltstream/releases/4b68b9a3867c
```

Consumer offset inspection:

```text
/var/lib/boltstream/consumer_offsets/livephase7/offsets.log
live-phase7-20260708200409	3	1	2242314251
live-phase7-20260708200409	0	2	518582248
live-phase7-20260708200409	1	2	522880479
live-phase7-20260708200409	2	2	493632390
live-phase7-20260708200409	3	2	481186225
```

Terraform drift after proof:

```text
terraform plan -detailed-exitcode -no-color
No changes. Your infrastructure matches the configuration.
terraform_drift=none
```

## Behavior Verified

- `/version` and build metadata report protocol version `3`.
- Protocol tests cover every new Phase 7 payload, frame name, error name,
  retryability, and malformed input path.
- `rebalance_required` is retryable and `stale_member` is non-retryable.
- `GroupCoordinator` maintains volatile membership keyed by `(group, topic)`.
- Member ids are generated as `member-000000000001` style values.
- Generations increment on membership join, leave, and timeout.
- Deterministic range assignment splits partitions contiguously by sorted member
  ids.
- Coordinated commits require the current member id, current generation id, and
  partition ownership.
- Legacy offset commits are rejected only while the same `(group, topic)` has an
  active coordinated group.
- Normal manual partition fetch/commit behavior remains available for
  non-coordinated groups.
- `boltstream-consumer --coordinated` emits JSON Lines join, assignment, record,
  commit, rebalance, leave, and summary events.
- The local and live smokes prove two consumers share a 4-partition topic, then
  the survivor receives all partitions after the other consumer stops
  heartbeating.
- Structured broker logs include join, rebalance, heartbeat, expiration, leave,
  coordinated commit, and coordinated commit rejection events with group, topic,
  member, generation, assignment, and partition fields where applicable.
