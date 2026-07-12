# BoltStream Storage

Storage format version `3` is a durable multi-partition, append-only log with
manifest-backed topic metadata, retention-aware low watermarks, consumer group offset
logs, and mixed version-1 record/version-2 batch entries. Existing format-2 data remains
readable; enabling compressed writes creates format-3 data that requires the current
binary or a pre-enable snapshot for rollback.
`boltstream-logtool` remains available for direct inspection and recovery checks.

## Directory Layout

```text
data/
  topics/
    trades/
      manifest.json
      partition-000000/
        00000000000000000000.log
        00000000000000000000.index
      partition-000001/
        00000000000000000000.log
        00000000000000000000.index
  consumer_offsets/
    dashboard/
      offsets.log
```

Topic names and consumer group names must match `[A-Za-z0-9._-]+` and must not
be `.` or `..`. Topics are created explicitly with an immutable partition count.
Legacy manifestless topics are imported as single-partition topics during broker
startup and receive a `manifest.json`.

Segment names use the segment base offset as a zero-padded 20-digit decimal
number. Segments roll by size and by active segment age. The default segment size
is `256 MiB`; the default segment age is one hour. Tests and operator smoke
checks can set smaller values through `--segment-bytes` and
`--segment-max-age-seconds`.

## Record Format

All integer fields are big-endian.

```text
uint32 record_bytes
uint16 record_version       // 1
uint16 flags                // 0 in format version 1
uint64 offset
uint64 timestamp_unix_ns
uint32 key_bytes
uint32 value_bytes
uint32 header_count         // 0 in format version 1
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

## Batch Format

Version-2 batch entries coexist with version-1 records in the same segment. All fields
are big-endian.

```text
uint32 batch_bytes
uint16 batch_version       // 2
uint16 flags               // 0
uint16 codec               // 0 none, 1 zstd
uint16 reserved            // 0
uint64 base_offset
uint64 timestamp_unix_ns
uint32 record_count
uint32 uncompressed_bytes
uint32 encoded_bytes
bytes  encoded_record_set
uint32 batch_crc32
```

The CRC covers the batch body from `batch_version` through the encoded record set. The
uncompressed record set contains its own count followed by length-prefixed key/value
pairs. Offsets are implicit and contiguous from `base_offset`; `next_offset` is
`base_offset + record_count`. The broker validates the codec, lengths, record count,
decompressed size, and CRC before exposing a batch.

## Recovery

Opening a partition scans segment files in base-offset order and decodes each entry by
its version. Recovery stops at the first incomplete record or batch, invalid header,
decompression failure, or CRC mismatch; it truncates the segment from that byte onward,
deletes later segments if necessary, rebuilds indexes, and sets `next_offset` to one
past the last valid recovered record or batch member.

Broker startup reads each topic manifest, opens all configured partition logs,
rebuilds indexes as needed, imports legacy manifestless `partition-000000` topics,
and writes recovery stats to `journalctl -u boltstream.service`. New topics are
created through `boltstream-admin topics create` or the binary `CreateTopicRequest`.

Consumer group offsets are stored in append-only text logs with CRC-protected records:

```text
topic<TAB>partition<TAB>next_offset<TAB>crc32<LF>
```

Offset recovery replays the latest offset per group/topic/partition and truncates
the first corrupt or incomplete trailing offset record.

## Retention

Retention deletes only inactive complete segments. The active segment is always
preserved even when it alone exceeds the configured byte limit. A retained
partition's low watermark is the first remaining indexed offset. Fetching
`beginning` starts at that low watermark. Fetching or committing an explicit
offset below it returns `offset_out_of_range`.

Global broker retention options:

```text
--retention-max-age-seconds 604800
--retention-max-bytes 1073741824
--retention-check-interval-ms 60000
```

Set a specific age or byte option to `0` to disable only that bound. Retention
runs at startup, after appends, on the periodic broker timer, and on explicit
`boltstream-admin retention run`.

## Operator Commands

```powershell
.\build\windows-gcc-debug\boltstream-logtool.exe append `
  --data .\data --topic trades --partition 0 --key AAPL --message "AAPL,100,192.41"

.\build\windows-gcc-debug\boltstream-logtool.exe read `
  --data .\data --topic trades --partition 0 --from 0 --max-records 10

.\build\windows-gcc-debug\boltstream-logtool.exe recover `
  --data .\data --topic trades --partition 0

.\build\windows-gcc-debug\boltstream-admin.exe retention run `
  --host 127.0.0.1 --port 9000 --topic trades
```

For deterministic process-crash recovery evidence covering a torn record, partial zstd
batch, and stale index:

```powershell
.\scripts\smoke-phase13.ps1 -BuildDir build/windows-gcc-debug
```

This proves recovery from deliberately incomplete filesystem mutations after committed
seed writes. It does not claim survival of unflushed page-cache data, physical power
loss, unrelated filesystem corruption, or disk/controller failure.
