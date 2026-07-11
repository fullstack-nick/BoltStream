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
- `boltstream-bench` public-batch smoke: schema `2`, codec `zstd`, batch size `32`,
  32 produced/fetched records, and zero errors. Phase 10's default schema remains `1`.
- Formatting check: passed.
- Docker Linux parity: GCC Debug build and 86/86 tests passed; GCC Release build
  passed. Builder image manifest: `sha256:93f2da092e62b36a0438f991d8303035b0351694f6dd6c435ea24e0f1e50cbd8`.

## CI and Exact Artifact

- Runtime commit: `ea7e0733c5b7dc96404b73450b7beb0c9464d909`.
- GitHub Actions run: `29163976416`, success for Linux Debug, Linux Release,
  Windows MSVC smoke, and Linux ThreadSanitizer. Both Linux Debug and Windows ran the
  checked-in Phase 11 compression smoke.
- Artifact: `boltstream-linux-x86_64-ea7e0733c5b7.tar.gz`, downloaded from that run.
- Artifact SHA-256:
  `936F92222B62B470EB12968B2FC1B089C6D7E20C713DBE14467EA6031B14E3D3`.

## Live GCP Evidence

- Target: project `boltstream-r7m5o9ld`, zone `us-central1-a`, instance
  `boltstream-vm`. The guarded deploy accepted only `nickaccturk@gmail.com`.
- Initial SCP timed out before VM mutation because Terraform still allowed the prior
  operator address `31.12.6.202/32`. The current address was `89.144.195.83/32`;
  Terraform updated only the SSH and broker operator firewall rules (`0 add, 2 change,
  0 destroy`) before the exact artifact was deployed.
- `/version` after deployment reports SHA `ea7e0733c5b7`, Release, GNU 13.3.0,
  protocol `5`, and storage format `3`.
- Normal-data startup after the full live regression recovered 105 existing records
  across 103 partitions and 33 topics with zero
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
- Structured append and fetch logs reported codec `zstd`, 8,580 logical bytes,
  58 encoded bytes, 102 stored bytes on append, and the pass-through batch metadata;
  no payload content was logged.
- The full existing live smoke subsequently passed public produce/fetch, durable
  commits, coordinated-group split/takeover, retention, offset-out-of-range, reset,
  topic deletion, and service restart on the exact Phase 11 artifact.
- Final state: `boltstream.service` active and ready; PID `58014`; memory
  `1,351,680` bytes; data disk 1% used with 19 GiB available; no Phase 11 directory,
  override, process, or `19100/19101` listener remained.
- Final `terraform plan -detailed-exitcode`: exit `0`, no changes.

## Rollback Boundary

The Phase 11 binary reads existing version-1 records and mixed version-1/version-2
batch segments. An older binary cannot read a directory after a format-3 batch is
written. Live proof therefore uses isolated Phase 11 data; enabling compression for
normal data requires retaining the Phase 11 binary or taking a pre-enable snapshot.
