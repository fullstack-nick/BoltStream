# Phase 12 Proof - Replication Simulation

## Scope

Phase 12 preserves public protocol version `5` and storage format `3`. It adds a
deterministic local two-broker replication simulator over real partition logs. Static
leader/follower assignment, canonical batch copying, leader/all acknowledgement modes,
lag metrics, offline behavior, and follower restart/resume are in scope. Elections,
promotion, quorum membership, consensus durability, and new listeners are excluded.

## Local Evidence - 2026-07-11

Windows Debug was configured with Clang 22.1.8, warnings as errors, tests enabled, and
benchmarks disabled:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DBOLTSTREAM_BUILD_TESTS=ON -DBOLTSTREAM_BUILD_BENCHMARKS=OFF
cmake --build build --target boltstream-replication-sim boltstream_tests -j 4
ctest --test-dir build --output-on-failure
```

Result: `89/89` tests passed. The Phase 12 subset was `4/4` after the final test audit.

The packaged simulator scenario was also run directly with zstd. Its stable result
showed leader and follower next offset `6`, lag `0`, two explicit replication fetches,
leader acknowledgement while the follower was offline, an observed all-replica timeout,
restart offset `4` before and after reopen, and exact logical record equality. Metrics
included leader/follower watermarks, zero lag, and follower availability with bounded
broker/topic/partition labels.

## Completion Gate

Local implementation evidence is complete. CI, exact pushed artifact packaging, isolated
live GCP execution, normal-service restoration, cleanup, disk headroom, and Terraform
drift evidence have not yet been captured. Phase 12 must not be called fully complete
until those live gates are recorded here.
