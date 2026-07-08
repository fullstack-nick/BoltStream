# Phase 5 Proof - Multi-Partition Topics and Consumer Groups

Date: 2026-07-08

Status: complete for runtime commit `3509559e7066`.

## Local Verification

Formatting and diff hygiene:

```text
.\scripts\format.ps1
git diff --check
```

`git diff --check` passed. Git reported line-ending warnings for PowerShell
scripts only.

Windows GCC tests:

```text
.\scripts\test.ps1 -Preset windows-gcc-debug
100% tests passed, 0 tests failed out of 38
```

Windows MSVC tests:

```text
.\scripts\test.ps1 -Preset windows-msvc-debug
100% tests passed, 0 tests failed out of 38
```

Windows Clang tests:

```text
.\scripts\test.ps1 -Preset windows-clang-debug
100% tests passed, 0 tests failed out of 38
```

Phase 5 CLI smoke:

```text
.\scripts\smoke-phase5.ps1 -Preset windows-gcc-debug
Phase 5 multi-partition/group/long-poll smoke passed on 127.0.0.1:4098.
```

Phase 4 compatibility smoke after explicit topic creation:

```text
.\scripts\smoke-phase4.ps1 -Preset windows-gcc-debug
Phase 4 broker produce/fetch smoke passed.
```

Windows release package:

```text
.\scripts\package.ps1 -Preset windows-msvc-release -GitSha 3509559e7066
Created C:\D_DRIVE\Nikita\JS\BoltStream\artifacts\boltstream-windows-x86_64-3509559e7066.zip
```

## GitHub CI

Implementation commit:

```text
3509559e7066ee8c15d8f2b9cd346a27937071ce Implement Phase 5 multi-partition topics
```

CI run `28937900459` passed for `3509559e7066ee8c15d8f2b9cd346a27937071ce`:

```text
Linux Debug: success
Linux Release: success
Windows MSVC smoke: success
```

The Windows CI job includes `scripts/smoke-phase5.ps1`.

The release artifact downloaded from that run was:

```text
artifacts/ci-phase5/boltstream-linux-x86_64-3509559e7066.tar.gz
```

## GCP Deploy And Live Proof

The deploy target was the existing Terraform-managed VM:

```text
project: boltstream-r7m5o9ld
zone: us-central1-a
instance: boltstream-vm
service: boltstream.service
```

The operator source CIDR had drifted because the public operator IP changed.
Terraform updated only the two managed operator firewall source ranges:

```text
terraform apply -auto-approve -no-color
Plan: 0 to add, 2 to change, 0 to destroy.
Apply complete! Resources: 0 added, 2 changed, 0 destroyed.
```

Deploy command:

```text
.\deployments\gcp\scripts\deploy.ps1 `
  -Artifact .\artifacts\ci-phase5\boltstream-linux-x86_64-3509559e7066.tar.gz `
  -GitSha 3509559e7066
```

Deploy result:

```text
Active: active (running)
/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
{"service":"boltstream","version":"0.1.0","git_sha":"3509559e7066","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"2","storage_format_version":"2","startup_time_utc":"2026-07-08T11:17:02Z"}
```

Authenticated live smoke:

```text
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha 3509559e7066 -BuildDir build\windows-gcc-debug
TCP broker port 9000 reachable.
Live broker produce/fetch succeeded for topic live-phase5-20260708112007.
Live broker group commit/resume succeeded for topic live-phase5-20260708112007.
```

Live health and version output:

```text
live:
{"service":"boltstream","status":"live","git_sha":"3509559e7066","detail":"ready"}
ready:
{"service":"boltstream","status":"ready","git_sha":"3509559e7066","detail":"ready"}
version:
{"service":"boltstream","version":"0.1.0","git_sha":"3509559e7066","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"2","storage_format_version":"2","startup_time_utc":"2026-07-08T11:17:02Z"}
```

Manual live group proof:

```json
{
  "topic": "live-phase5-groups-20260708111849",
  "partition": 1,
  "produce": {
    "status": "ok",
    "offset": 0,
    "next_offset": 1
  },
  "commit": {
    "status": "ok",
    "from": 0,
    "count": 1,
    "committed_offset": 1
  },
  "resume": {
    "status": "ok",
    "from": 1,
    "count": 0,
    "next_offset": 1
  }
}
```

Live manifest and partition files:

```text
/var/lib/boltstream/topics/live-phase5-groups-20260708111849/manifest.json
/var/lib/boltstream/topics/live-phase5-groups-20260708111849/partition-000000/00000000000000000000.index
/var/lib/boltstream/topics/live-phase5-groups-20260708111849/partition-000000/00000000000000000000.log
/var/lib/boltstream/topics/live-phase5-groups-20260708111849/partition-000001/00000000000000000000.index
/var/lib/boltstream/topics/live-phase5-groups-20260708111849/partition-000001/00000000000000000000.log
/var/lib/boltstream/topics/live-phase5-groups-20260708111849/partition-000002/00000000000000000000.index
/var/lib/boltstream/topics/live-phase5-groups-20260708111849/partition-000002/00000000000000000000.log
```

Manifest contents:

```json
{
  "manifest_version": 1,
  "topic": "live-phase5-groups-20260708111849",
  "partition_count": 3,
  "created_at_utc": "2026-07-08T11:18:50Z"
}
```

Live consumer offset log:

```text
/var/lib/boltstream/consumer_offsets/phase5live/offsets.log
live-phase5-groups-20260708111849	1	1	1293640263

/var/lib/boltstream/consumer_offsets/livephase5/offsets.log
live-phase5-20260708112007	1	1	3120271526
```

Journal proof after live calls:

```text
protocol request correlation_id=2 type=create_topic_request payload_bytes=32
protocol request correlation_id=2 type=produce_request payload_bytes=57
protocol request correlation_id=2 type=fetch_request payload_bytes=53
protocol request correlation_id=2 type=fetch_request payload_bytes=63
protocol request correlation_id=3 type=offset_commit_request payload_bytes=54
protocol request correlation_id=2 type=fetch_request payload_bytes=63
```

SSH inspection:

```text
.\deployments\gcp\scripts\inspect-live.ps1
Active: active (running)
data dir writable by boltstream
/var/lib/boltstream/consumer_offsets/livephase5/offsets.log
/opt/boltstream/releases/3509559e7066
```

Terraform drift after proof:

```text
terraform plan -detailed-exitcode -no-color
No changes. Your infrastructure matches the configuration.
```

## Behavior Verified

- Topics are explicitly created with immutable partition counts before produce.
- The broker rejects lazy produce to unknown topics.
- Multi-partition topics create a manifest plus partition `0..N-1` log/index directories.
- Keyed produce routes to a stable partition and empty-key produce round-robins locally.
- Fetch is partition-aware and supports `beginning`, `latest`, explicit offsets, and `committed`.
- Consumer group commits store the next offset, survive restart locally, and resume live.
- Long-poll fetch is nonblocking locally and completes when a delayed produce arrives.
- Phase 4 manifestless data remains readable as single-partition topics after recovery.
- Auth is required on the deployed broker for create-topic, produce, fetch, and commit.
- CI now runs the Phase 5 CLI smoke in the Windows job.

## Completion Decision

Phase 5 is complete for runtime commit `3509559e7066`: the implementation is
pushed, CI is green, the exact CI Linux artifact is deployed to GCP, live
create-topic/produce/fetch/commit/resume calls pass, SSH inspection proves
manifests, partition logs, offset logs, systemd, journal, release contents, and
runtime version, and Terraform reports no drift after the firewall source-range
refresh.
