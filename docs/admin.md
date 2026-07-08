# BoltStream Admin

Phase 5 adds the first admin command: explicit topic creation through the binary
broker protocol. Topic list, describe, delete, consumer group describe, and offset
reset commands are reserved for the retention and lifecycle phase.

## Create Topic

```powershell
.\build\windows-gcc-debug\boltstream-admin.exe topics create `
  --host 127.0.0.1 --port 9000 `
  --topic trades --partitions 3
```

The command returns one JSON line:

```json
{"status":"created","topic":"trades","partitions":3,"correlation_id":1}
```

Creating the same topic again with the same partition count is idempotent and
returns `status: "exists"`. Creating it with a different partition count returns a
`topic_conflict` error response.

When `BOLTSTREAM_BROKER_TOKEN` is configured on the broker, pass `--token TOKEN` or
set the same environment variable for the admin CLI.

## Current Boundaries

- Topic partition counts are immutable.
- Producers do not create topics lazily.
- One-shot consumers choose partitions explicitly.
- Coordinated consumers use broker-managed membership, heartbeats, automatic
  assignment, generation fencing, and rebalancing for one topic per `(group,
  topic)` coordinator instance.
- Topic deletion, retention, group inspection, group describe, and offset reset
  are planned for the retention and lifecycle phase.
