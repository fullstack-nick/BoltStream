# BoltStream Protocol

BoltStream broker/client traffic uses a custom binary TCP protocol. Protocol version
`1` validates framed requests and preserves correlation ids; broker produce/fetch
success responses are implemented in Phase 4.

## Frame Header

Every frame begins with this fixed 32-byte header. All integer fields use network byte
order.

```text
uint32 magic          0x42535452 ("BSTR")
uint16 version        1
uint16 frame_type
uint32 header_bytes   32
uint32 payload_bytes
uint64 correlation_id
uint32 flags          0 in protocol version 1
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

The broker accepts `HealthRequest`, `MetadataRequest`, `ProduceRequest`, `FetchRequest`,
`OffsetCommitRequest`, and `AuthRequest`. Health returns `HealthResponse`. The remaining
valid requests return `ErrorResponse` with `not_implemented` until the broker behavior
phase lands.

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
- `ProduceRequest`: `string topic`, `bytes key`, `bytes message`.
- `FetchRequest`: `string topic`, `string from`.
- `OffsetCommitRequest` and `AuthRequest`: empty.
- `ErrorResponse`: `uint32 error_code`, `string message`.

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

Malformed or unsafe frames receive a structured `ErrorResponse` when possible and then
the broker closes the connection. Valid but unsupported operations keep the
connection open and return `not_implemented` with the original correlation id.

## CLI Behavior

The producer and consumer CLIs use the same C++ async client library as other clients:

```powershell
.\build\windows-gcc-debug\boltstream-producer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --key AAPL --message "AAPL,100,192.41"

.\build\windows-gcc-debug\boltstream-consumer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --from beginning
```

Until Phase 4 implements broker produce/fetch, both commands return exit code `3` and a
structured JSON line with `"status":"not_implemented"`.
