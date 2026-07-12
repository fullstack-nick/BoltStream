# Phase 12 Proof - Replication Simulation

## Scope

Phase 12 preserves public protocol version `5` and storage format `3`. It adds a
deterministic local two-broker replication simulator over real partition logs. Static
leader/follower assignment, canonical batch copying, leader/all acknowledgement modes,
lag metrics, offline behavior, and follower restart/resume are in scope. Elections,
promotion, quorum membership, consensus durability, and new listeners are excluded.

## Local Evidence - 2026-07-11/12

Windows Debug was configured with Clang 22.1.8, warnings as errors, tests enabled, and
benchmarks disabled:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DBOLTSTREAM_BUILD_TESTS=ON -DBOLTSTREAM_BUILD_BENCHMARKS=OFF
cmake --build build --target boltstream-replication-sim boltstream_tests -j 4
ctest --test-dir build --output-on-failure
```

Result: `90/90` tests passed. The Phase 12 subset was `4/4` after the final test audit.

The packaged simulator scenario was also run directly with zstd. Its stable result
showed leader and follower next offset `6`, lag `0`, two explicit replication fetches,
leader acknowledgement while the follower was offline, an observed all-replica timeout,
restart offset `4` before and after reopen, and exact logical record equality. Metrics
included leader/follower watermarks, zero lag, and follower availability with bounded
broker/topic/partition labels.

## CI and Exact Artifact - 2026-07-12

- Runtime commit: `55a3fd3dcfa68f2e5d8bacf69cc47e5c67c48c8d`.
- GitHub Actions run: `29178095052`, successful for Linux Debug, Linux Release,
  Windows MSVC smoke, and Linux ThreadSanitizer.
- The checked-in Phase 12 smoke passed on both Linux Debug and Windows MSVC.
- Artifact: `boltstream-linux-x86_64-55a3fd3dcfa6.tar.gz`, downloaded directly from
  the successful run. It contains both `boltstream-server` and
  `boltstream-replication-sim`.
- Artifact SHA-256:
  `BB631328BF3C410891148E2486DEB25E73CEAED5DD64F23EFF74D32F93FFB297`.

## Live GCP Evidence - 2026-07-12

- Target: project `boltstream-r7m5o9ld`, zone `us-central1-a`, instance
  `boltstream-vm`. Both deploy and live-proof helpers accepted only the active account
  `nickaccturk@gmail.com`.
- The exact CI artifact was installed at `/opt/boltstream/releases/55a3fd3dcfa6` and
  selected by `/opt/boltstream/current`.
- `/version` reports SHA `55a3fd3dcfa6`, Release, GNU 13.3.0, protocol `5`, and
  storage format `3`. Normal startup recovered 105 existing records across 103
  partitions and 33 topics with zero truncated bytes.
- `deployments/gcp/scripts/smoke-phase12-live.ps1` ran the packaged simulator as the
  unprivileged `boltstream` user under isolated `/var/lib/boltstream/phase12-live`:
  - leader and follower reached next offset `6` with lag `0`;
  - two follower fetch operations copied the missing batches;
  - leader acknowledgement succeeded while the follower was unavailable;
  - all-replica acknowledgement timed out while unavailable and later succeeded;
  - the follower recovered offset `4` before and after reopen;
  - logical records matched exactly after zstd replication;
  - leader and follower logs were both 234 bytes and indexes were both 120 bytes;
  - bounded Prometheus output reported leader/follower watermarks, zero lag, and
    follower availability.
- Cleanup removed the isolated Phase 12 directory and remote proof script. The normal
  service remained active and ready on PID `59670`; no service override or listener was
  introduced. The data disk remained 1% used with 19 GiB available.
- Final `terraform plan -detailed-exitcode -input=false -no-color`: exit `0`, no
  changes.

## Completion Gate

Phase 12 is complete locally, in cross-platform CI, in the exact packaged artifact, and
on the isolated live GCP proof path. The normal broker is restored, healthy, and running
the exact runtime commit; disposable replication state is absent and Terraform has no
drift.
