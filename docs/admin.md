# BoltStream Admin

`boltstream-admin` uses the same authenticated binary broker protocol as the
producer and consumer CLIs. It is the supported operator path for topic lifecycle,
retention, and consumer group offset inspection.

If `BOLTSTREAM_BROKER_TOKEN` is configured on the broker, pass `--token TOKEN` or
set the same environment variable for the admin CLI.

## Topics

```powershell
.\build\windows-gcc-debug\boltstream-admin.exe topics create `
  --host 127.0.0.1 --port 9000 `
  --topic trades --partitions 3

.\build\windows-gcc-debug\boltstream-admin.exe topics list `
  --host 127.0.0.1 --port 9000

.\build\windows-gcc-debug\boltstream-admin.exe topics describe `
  --host 127.0.0.1 --port 9000 `
  --topic trades

.\build\windows-gcc-debug\boltstream-admin.exe topics delete `
  --host 127.0.0.1 --port 9000 `
  --topic trades
```

Topic creation is idempotent when the partition count matches and returns
`status: "exists"`. A different partition count returns `topic_conflict`.

Topic delete removes the manifest, partition logs, indexes, and inactive group
offset entries for that topic. Delete rejects topics with active coordinated group
members and returns `group_active`.

## Retention

```powershell
.\build\windows-gcc-debug\boltstream-admin.exe retention run `
  --host 127.0.0.1 --port 9000 `
  --topic trades
```

Omit `--topic` to scan all topics. Retention deletes only inactive complete
segments. It preserves the active segment, updates the partition low watermark,
and returns deleted segment/byte counts plus each scanned partition's earliest and
next offsets.

## Consumer Groups

```powershell
.\build\windows-gcc-debug\boltstream-admin.exe groups describe `
  --host 127.0.0.1 --port 9000 `
  --group dashboard --topic trades

.\build\windows-gcc-debug\boltstream-admin.exe groups reset-offset `
  --host 127.0.0.1 --port 9000 `
  --group dashboard --topic trades --partition 0 --to beginning
```

`groups describe` reports committed offsets, partition earliest offsets, next
offsets, lag, out-of-range status, and active coordinated member count.

`groups reset-offset` accepts `beginning`, `latest`, or an unsigned offset. It
rejects active coordinated groups with `group_active`, rejects targets below the
partition low watermark with `offset_out_of_range`, and rejects targets beyond
the partition high watermark with `invalid_offset`.
