# Phase 11 Compression Smoke

This is one functional comparison from native Windows GCC Debug, not a capacity
benchmark or statistically meaningful performance recommendation. Each mode sends and
fetches one public batch of 32 records containing the same deterministic 260-byte
compressible payload. zstd uses level 3.

| Codec | Produce records/s | Batch ack latency (ms) | Fetch records/s | Logical bytes | Encoded bytes | Partition log bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| none | 312.560 | 102.380 | 671.723 | 8,580 | 8,580 | 8,624 |
| zstd | 485.263 | 65.944 | 1,190.870 | 8,580 | 58 | 102 |

The timings include CLI process startup and contain only one sample, so they prove the
measurement path rather than relative runtime performance. The byte comparison is the
acceptance signal for this deliberately compressible fixture: the broker stored the
producer's exact zstd record set, and the capable consumer incremented the compressed
fetch pass-through counter.

The canonical machine-readable result is
[`benchmarks/results/phase-11-windows-smoke.json`](../benchmarks/results/phase-11-windows-smoke.json).
The same checked-in smoke command is used for the isolated GCP proof.
