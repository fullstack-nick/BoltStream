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

## CI, Artifact, and GCP Evidence

Pending the implementation commit, exact-SHA CI run, Release artifact deployment,
isolated live smoke, cleanup inspection, and Terraform drift check. This phase is not
complete until these fields are replaced with current evidence.

## Rollback Boundary

The Phase 11 binary reads existing version-1 records and mixed version-1/version-2
batch segments. An older binary cannot read a directory after a format-3 batch is
written. Live proof therefore uses isolated Phase 11 data; enabling compression for
normal data requires retaining the Phase 11 binary or taking a pre-enable snapshot.
