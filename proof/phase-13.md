# Phase 13 Proof - Crash Recovery

## Scope

Phase 13 preserves public protocol version `5` and storage format `3`. It adds a
packaged parent/worker crash harness for three deterministic active-tail failures: a
torn version-1 record, a partial version-2 zstd batch, and a partial/stale index append.
The worker flushes three committed seed records, stages the incomplete mutation, and
terminates with `_Exit(86)`. The parent requires abnormal termination and verifies the
complete recovered logical log.

This proves BoltStream's process-crash and torn-file recovery contract after bytes reach
the filesystem interface. It is not a claim about unflushed kernel page-cache data,
disk-controller loss, arbitrary filesystem damage, or physical power failure.

## Local Evidence - 2026-07-12

- Native Windows GCC Debug: `93/93` tests passed; Phase 13 focused subset `3/3`.
- Native Windows MSVC Debug: `93/93` tests passed; Phase 13 smoke and focused subset
  passed.
- Native Windows Clang Debug: `93/93` tests passed.
- Formatting and `git diff --check` passed.
- The process-crash smoke produced the same stable physical results as the live run:
  torn record truncated 30 bytes, partial zstd batch truncated 57 bytes, and the
  stale-index scenario truncated no valid log bytes. Every case recovered exactly three
  records at offsets `0..2`, restored `next_offset=3`, rebuilt one index, and left a
  60-byte canonical index.

## CI and Exact Artifact - 2026-07-12

- Runtime commit: `5e7635a442e8f099b35b666b5e4fea79723108b8`.
- GitHub Actions run: `29178882592`, successful for Linux Debug, Linux Release,
  Windows MSVC smoke, and Linux ThreadSanitizer.
- Linux Debug passed all 93 tests and the checked-in Phase 13 process-crash smoke.
  Windows MSVC passed the Phase 13 smoke as well.
- Artifact: `boltstream-linux-x86_64-5e7635a442e8.tar.gz`, downloaded directly from
  run `29178882592`. Its manifest contains `boltstream-server` and
  `boltstream-recovery-proof`.
- Artifact SHA-256:
  `64F9450A3524E7D9109D686BE6C7A6B2C2C64876DB96C6C7D80AF723C8F539D9`.

## Live GCP Evidence - 2026-07-12

- Target: project `boltstream-r7m5o9ld`, zone `us-central1-a`, instance
  `boltstream-vm`. The checked-in helpers accepted only active account
  `nickaccturk@gmail.com`.
- The exact CI artifact was installed at `/opt/boltstream/releases/5e7635a442e8` and
  selected by `/opt/boltstream/current`.
- `/version` reports SHA `5e7635a442e8`, Release, GNU 13.3.0, protocol `5`, and
  storage format `3`. Normal startup recovered 105 existing records across 103
  partitions and 33 topics with zero truncated bytes.
- `deployments/gcp/scripts/smoke-phase13-live.ps1` ran the packaged proof executable as
  the unprivileged `boltstream` user under isolated
  `/var/lib/boltstream/phase13-live` state:
  - all three workers terminated abnormally;
  - each reopen recovered exactly three committed records and `next_offset=3`;
  - the torn record truncated 30 bytes and the partial zstd batch truncated 57 bytes;
  - the index-only failure truncated zero log bytes;
  - all three indexes were rebuilt from the canonical logs to exactly 60 bytes;
  - no partial, duplicate, or phantom record survived.
- The first live-helper attempt exposed a proof-script assertion defect: it expected
  numeric protocol/storage JSON fields although `/version` returns strings. The helper
  was corrected to match the stable endpoint schema, made to emit remote output before
  raising an SSH failure, and rerun successfully against the unchanged exact artifact.
- Cleanup removed the isolated Phase 13 directories, the diagnostic directory, and the
  uploaded helper. The normal service remained `active/running` on PID `60485`, ready on
  the exact runtime SHA. The data disk remained 1% used with 19 GiB available.
- Final `terraform plan -detailed-exitcode -input=false -no-color`: exit `0`, no
  changes.

## Completion Gate

Phase 13 is complete locally, in cross-platform CI, in the exact packaged artifact, and
on the isolated live GCP crash/recovery path. The normal broker is healthy on the exact
runtime commit, disposable proof state is absent, and Terraform has no drift.
