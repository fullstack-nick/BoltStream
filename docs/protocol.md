# BoltStream Protocol

BoltStream broker/client traffic uses a custom binary TCP protocol. Protocol version
`2` validates framed requests, preserves correlation ids, supports explicit topic
creation, multi-partition produce/fetch, durable consumer offset commits, long-poll
fetch, and retryable overload responses.

## Frame Header

Every frame begins with this fixed 32-byte header. All integer fields use network byte
order.

```text
uint32 magic          0x42535452 ("BSTR")
uint16 version        2
uint16 frame_type
uint32 header_bytes   32
uint32 payload_bytes
uint64 correlation_id
uint32 flags          0 in protocol version 2
uint32 header_crc32   CRC32 over the first 28 header bytes
```

The broker enforces `--max-frame-bytes`, defaulting to `1048576`. The limit includes the
32-byte header and payload bytes.

## Frame Types

| Id | Name |
| ---: | --- |
| 1 | `ErrorResponse` |
| 2 | `HealthRequest` |
| 3 | `HealthResponse` |
| 4 | `MetadataRequest` |
| 5 | `MetadataResponse` |
| 6 | `ProduceRequest` |
| 7 | `ProduceResponse` |
| 8 | `FetchRequest` |
| 9 | `FetchResponse` |
| 10 | `OffsetCommitRequest` |
| 11 | `OffsetCommitResponse` |
| 12 | `AuthRequest` |
| 13 | `AuthResponse` |
| 14 | `CreateTopicRequest` |
| 15 | `CreateTopicResponse` |

The broker accepts `HealthRequest`, `MetadataRequest`, `CreateTopicRequest`,
`ProduceRequest`, `FetchRequest`, `OffsetCommitRequest`, and `AuthRequest`. Health
returns `HealthResponse`. Metadata, create-topic, produce, fetch, commit, and auth
return success frames when valid.

## Payload Encoding

Strings and byte arrays use a 32-bit length prefix followed by raw bytes:

```text
uint32 byte_length
uint8[byte_length] bytes
```

Payloads:

- `HealthRequest`: empty.
- `HealthResponse`: `string status`, `string detail`.
- `MetadataRequest`: empty.
- `MetadataResponse`: `uint32 topic_count`, then repeated `string topic`, `uint16 partition`, `uint64 next_offset`.
- `CreateTopicRequest`: `string topic`, `uint16 partition_count`.
- `CreateTopicResponse`: `string topic`, `uint16 partition_count`, `string status`.
- `ProduceRequest`: `string topic`, `bytes key`, `bytes message`.
- `ProduceResponse`: `string topic`, `uint16 partition`, `uint64 offset`, `uint64 next_offset`, `uint32 encoded_bytes`.
- `FetchRequest`: `string topic`, `uint16 partition`, `string from`, `string group`, `uint32 max_wait_ms`.
- `FetchResponse`: `string topic`, `uint16 partition`, `uint64 from_offset`, `uint64 next_offset`, `uint32 record_count`, then repeated `uint64 offset`, `uint64 timestamp_unix_ns`, `bytes key`, `bytes message`, `uint32 encoded_bytes`.
- `AuthRequest`: `string token`.
- `AuthResponse`: `string status`.
- `OffsetCommitRequest`: `string group`, `string topic`, `uint16 partition`, `uint64 next_offset`.
- `OffsetCommitResponse`: `string group`, `string topic`, `uint16 partition`, `uint64 next_offset`.
- `ErrorResponse`: `uint32 error_code`, `string message`.

`FetchRequest.from` supports `beginning`, `latest`, `committed`, or an unsigned offset
encoded as decimal ASCII. `committed` requires a non-empty group. Long-poll waits only
when the resolved fetch offset equals the partition high watermark; otherwise the broker
returns immediately. `FetchResponse.next_offset` is the resume offset for the next fetch:
when a response is truncated by record or byte caps, it is one past the final returned
record. Use metadata for the partition high watermark.

## Error Codes

| Id | Name |
| ---: | --- |
| 1 | `invalid_magic` |
| 2 | `unsupported_version` |
| 3 | `invalid_length` |
| 4 | `crc_mismatch` |
| 5 | `malformed_payload` |
| 6 | `unsupported_request` |
| 7 | `not_implemented` |
| 8 | `internal_error` |
| 9 | `reserved_flags` |
| 10 | `unauthorized` |
| 11 | `unknown_topic` |
| 12 | `topic_conflict` |
| 13 | `invalid_partition` |
| 14 | `invalid_group` |
| 15 | `invalid_offset` |
| 16 | `overloaded` |

Malformed or unsafe frames receive a structured `ErrorResponse` when possible and then
the broker closes the connection. Valid but unsupported operations keep the connection
open and return `not_implemented` with the original correlation id. If
`BOLTSTREAM_BROKER_TOKEN` is configured, metadata, produce, and fetch require a prior
successful `AuthRequest`; unauthorized requests return `unauthorized` and close the
connection. Create-topic and offset commit are protected by the same auth gate. Health
remains unauthenticated.

`overloaded` is retryable. The broker returns it when bounded append queues or long-poll
waiter slots are exhausted. The producer, consumer, and admin CLIs include
`"retryable": true` in overload error JSON and exit with code `5`.

## CLI Behavior

The producer and consumer CLIs use the same C++ async client library as other clients:

```powershell
.\build\windows-gcc-debug\boltstream-admin.exe topics create `
  --host 127.0.0.1 --port 9000 `
  --topic trades --partitions 3

.\build\windows-gcc-debug\boltstream-producer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --key AAPL --message "AAPL,100,192.41"

.\build\windows-gcc-debug\boltstream-consumer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --partition 0 --from beginning

.\build\windows-gcc-debug\boltstream-consumer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --partition 0 --group dashboard --commit
```

Both commands return exit code `0` on success and print one structured JSON line. The
producer includes the assigned offset and next offset. The consumer includes the
returned records and next offset.

When the broker requires auth, pass `--token TOKEN` or set `BOLTSTREAM_BROKER_TOKEN`
before running producer or consumer.
