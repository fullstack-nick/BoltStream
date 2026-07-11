# Phase 11 Proof - Compression

## Contract

Phase 11 is protocol version `5` and storage format `3`. It adds producer-side zstd
batch compression, bounded broker validation, exact compressed storage, v5 metadata
capability negotiation, aligned compressed fetch pass-through, and mixed legacy/batch
recovery. Protocol-v4 clients remain accepted. Replication is not included.

## Local Evidence

- zstd `1.5.7` is SHA-256-pinned and built statically with programs, tests, shared
  libraries, legacy support, and multithreaded compression disabled.
- Native Windows GCC Debug and MSVC Debug builds: passed.
- CTest: 86/86 passed, including compression bounds, batch protocol encoding, mixed
  legacy/zstd recovery, and v4/v5 metadata shapes.
- `scripts/smoke-phase11.ps1`: passed with 32 exact records, negotiated zstd produce
  and fetch, pass-through metric `1`, and physical log comparison of 8,624 bytes for
  `none` versus 102 bytes for zstd on the deterministic compressible payload.
- Formatting check: passed.
- Docker Linux parity: GCC Debug build and 86/86 tests passed; GCC Release build
  passed. Builder image manifest: `sha256:93f2da092e62b36a0438f991d8303035b0351694f6dd6c435ea24e0f1e50cbd8`.

## CI and Exact Artifact

- Runtime commit: `173e9a72e1711719270629113e0bffcf7490b82b`.
- GitHub Actions run: `29157056192`, success for Linux Debug, Linux Release,
  Windows MSVC smoke, and Linux ThreadSanitizer. Both Linux Debug and Windows ran the
  checked-in Phase 11 compression smoke.
- Artifact: `boltstream-linux-x86_64-173e9a72e171.tar.gz`, downloaded from that run.
- Artifact SHA-256:
  `4BD28DE87FB7C2AEE3714686FF08288E35A2E0A24E310694CE0CA1F956BA5727`.

## Live GCP Evidence

- Target: project `boltstream-r7m5o9ld`, zone `us-central1-a`, instance
  `boltstream-vm`. The guarded deploy accepted only `nickaccturk@gmail.com`.
- Initial SCP timed out before VM mutation because Terraform still allowed the prior
  operator address `31.12.6.202/32`. The current address was `89.144.195.83/32`;
  Terraform updated only the SSH and broker operator firewall rules (`0 add, 2 change,
  0 destroy`) before the exact artifact was deployed.
- `/version` after deployment reports SHA `173e9a72e171`, Release, GNU 13.3.0,
  protocol `5`, and storage format `3`.
- Normal-data startup recovered 87 existing records across 89 partitions with zero
  truncated bytes, proving forward readability of the deployed storage-v2 data.
- The isolated loopback proof used ports `19100/19101` and
  `/var/lib/boltstream/phase11-live`:
  - both modes produced and fetched exactly 32 records;
  - `none`: 8,580 logical/encoded bytes and 8,624 physical log bytes;
  - `zstd`: 8,580 logical bytes, 58 encoded bytes, and 102 physical log bytes;
  - `boltstream_compressed_fetch_passthrough_total` increased to `1`;
  - `boltstream-logtool inspect-batch` reported codec `zstd`, offsets `0..32`, and
    ratio `0.00675991`;
  - recovery rebuilt the index for all 32 records with zero truncation.
- The full existing live smoke subsequently passed public produce/fetch, durable
  commits, coordinated-group split/takeover, retention, offset-out-of-range, reset,
  topic deletion, and service restart on the exact Phase 11 artifact.
- Final state: `boltstream.service` active and ready; PID `56326`; memory
  `1,556,480` bytes; data disk 1% used with 19 GiB available; no Phase 11 directory,
  override, process, or `19100/19101` listener remained.
- Final `terraform plan -detailed-exitcode`: exit `0`, no changes.

## Rollback Boundary

The Phase 11 binary reads existing version-1 records and mixed version-1/version-2
batch segments. An older binary cannot read a directory after a format-3 batch is
written. Live proof therefore uses isolated Phase 11 data; enabling compression for
normal data requires retaining the Phase 11 binary or taking a pre-enable snapshot.
