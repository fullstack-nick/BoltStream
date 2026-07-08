# BoltStream Storage

Phase 3 implements storage format version `1`: a durable single-partition,
append-only log. Broker `ProduceRequest` and `FetchRequest` still return
`not_implemented` until Phase 4; storage is operated directly through
`boltstream-logtool` and recovered during broker startup.

## Directory Layout

```text
data/
  topics/
    trades/
      partition-000000/
        00000000000000000000.log
        00000000000000000000.index
```

Only partition `0` exists in Phase 3. Topic names must match
`[A-Za-z0-9._-]+` and must not be `.` or `..`.

Segment names use the segment base offset as a zero-padded 20-digit decimal
number. Segments roll by size. The default segment size is `256 MiB`; tests and
operator smoke checks can set a smaller value through `--segment-bytes`.

## Record Format

All integer fields are big-endian.

```text
uint32 record_bytes
uint16 record_version       // 1
uint16 flags                // 0 in Phase 3
uint64 offset
uint64 timestamp_unix_ns
uint32 key_bytes
uint32 value_bytes
uint32 header_count         // 0 in Phase 3
bytes  key
bytes  value
uint32 record_crc32
```

`record_bytes` is the byte count after the length field, including the trailing
CRC. The CRC32 covers the record body from `record_version` through `value`,
excluding `record_bytes` and excluding the CRC field itself.

## Index Format

Each `.index` file is rebuilt from its matching `.log` file on open. Index
entries are fixed width:

```text
uint64 offset
uint64 file_position
uint32 record_bytes
```

The index is an acceleration structure only. If an index is missing or stale,
recovery rewrites it from the log.

## Recovery

Opening a partition scans segment files in base-offset order. Recovery stops at
the first incomplete record, invalid record, or CRC mismatch, truncates the
segment from that byte onward, deletes later segments if necessary, rebuilds
indexes, and sets `next_offset` to one past the last valid recovered record.

Broker startup runs recovery for existing `topics/*/partition-000000` logs and
writes recovery stats to `journalctl -u boltstream.service`.

## Operator Commands

```powershell
.\build\windows-gcc-debug\boltstream-logtool.exe append `
  --data .\data --topic trades --key AAPL --message "AAPL,100,192.41"

.\build\windows-gcc-debug\boltstream-logtool.exe read `
  --data .\data --topic trades --from 0 --max-records 10

.\build\windows-gcc-debug\boltstream-logtool.exe recover `
  --data .\data --topic trades
```

For a repeatable local corruption/recovery check:

```powershell
.\scripts\smoke-phase3.ps1 -Preset windows-gcc-debug
```
