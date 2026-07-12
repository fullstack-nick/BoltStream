#!/usr/bin/env python3
"""Zero-dependency BoltStream reference client and command-line demo."""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import zlib
from dataclasses import asdict, dataclass
from typing import Optional

MAGIC = 0x42535452
PROTOCOL_VERSION = 4
HEADER_BYTES = 32
MAX_FRAME_BYTES = 1024 * 1024

ERROR_RESPONSE = 1
HEALTH_REQUEST, HEALTH_RESPONSE = 2, 3
PRODUCE_REQUEST, PRODUCE_RESPONSE = 6, 7
FETCH_REQUEST, FETCH_RESPONSE = 8, 9
AUTH_REQUEST, AUTH_RESPONSE = 12, 13
CREATE_TOPIC_REQUEST, CREATE_TOPIC_RESPONSE = 14, 15


class BoltStreamError(RuntimeError):
    """Base client failure."""


class ProtocolError(BoltStreamError):
    """Malformed or unexpected protocol data."""


class BrokerError(BoltStreamError):
    """Structured error returned by the broker."""

    def __init__(self, code: int, message: str) -> None:
        self.code = code
        self.message = message
        super().__init__(f"broker error {code}: {message}")


@dataclass(frozen=True)
class Record:
    offset: int
    timestamp_unix_ns: int
    key: str
    message: str
    encoded_bytes: int


class _Reader:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.offset = 0

    def _take(self, size: int) -> bytes:
        end = self.offset + size
        if size < 0 or end > len(self.payload):
            raise ProtocolError("truncated response payload")
        value = self.payload[self.offset:end]
        self.offset = end
        return value

    def u16(self) -> int:
        return struct.unpack(">H", self._take(2))[0]

    def u32(self) -> int:
        return struct.unpack(">I", self._take(4))[0]

    def u64(self) -> int:
        return struct.unpack(">Q", self._take(8))[0]

    def raw(self) -> bytes:
        return self._take(self.u32())

    def text(self) -> str:
        try:
            return self.raw().decode("utf-8")
        except UnicodeDecodeError as error:
            raise ProtocolError("response contains invalid UTF-8") from error

    def finish(self) -> None:
        if self.offset != len(self.payload):
            raise ProtocolError("response payload contains trailing bytes")


def _u16(value: int) -> bytes:
    return struct.pack(">H", value)


def _u32(value: int) -> bytes:
    return struct.pack(">I", value)


def _bytes(value: bytes) -> bytes:
    return _u32(len(value)) + value


def _text(value: str) -> bytes:
    return _bytes(value.encode("utf-8"))


def encode_frame(frame_type: int, correlation_id: int, payload: bytes = b"") -> bytes:
    if len(payload) + HEADER_BYTES > MAX_FRAME_BYTES:
        raise ProtocolError("request exceeds maximum frame size")
    prefix = struct.pack(
        ">IHHIIQI", MAGIC, PROTOCOL_VERSION, frame_type, HEADER_BYTES,
        len(payload), correlation_id, 0,
    )
    return prefix + _u32(zlib.crc32(prefix) & 0xFFFFFFFF) + payload


def decode_frame(frame: bytes, expected_correlation_id: Optional[int] = None) -> tuple[int, bytes]:
    if len(frame) < HEADER_BYTES:
        raise ProtocolError("truncated frame header")
    magic, version, frame_type, header_bytes, payload_bytes, correlation_id, flags, crc = (
        struct.unpack(">IHHIIQII", frame[:HEADER_BYTES])
    )
    if magic != MAGIC:
        raise ProtocolError("invalid frame magic")
    if version not in (4, 5):
        raise ProtocolError(f"unsupported protocol version {version}")
    if header_bytes != HEADER_BYTES or payload_bytes > MAX_FRAME_BYTES - HEADER_BYTES:
        raise ProtocolError("invalid frame length")
    if flags != 0:
        raise ProtocolError("reserved frame flags are set")
    if zlib.crc32(frame[:28]) & 0xFFFFFFFF != crc:
        raise ProtocolError("frame header CRC mismatch")
    if len(frame) != HEADER_BYTES + payload_bytes:
        raise ProtocolError("frame size does not match payload length")
    if expected_correlation_id is not None and correlation_id != expected_correlation_id:
        raise ProtocolError("response correlation id does not match request")
    return frame_type, frame[HEADER_BYTES:]


class Client:
    def __init__(self, host: str = "127.0.0.1", port: int = 9000,
                 token: Optional[str] = None, timeout: float = 5.0) -> None:
        self.host = host
        self.port = port
        self.token = token
        self.timeout = timeout
        self._socket: Optional[socket.socket] = None
        self._correlation_id = 0

    def __enter__(self) -> "Client":
        self.connect()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def connect(self) -> None:
        if self._socket is not None:
            return
        self._socket = socket.create_connection((self.host, self.port), self.timeout)
        self._socket.settimeout(self.timeout)
        if self.token:
            reader = _Reader(self._request(AUTH_REQUEST, AUTH_RESPONSE, _text(self.token)))
            status = reader.text()
            reader.finish()
            if status != "authenticated":
                raise ProtocolError(f"unexpected authentication status {status!r}")

    def close(self) -> None:
        if self._socket is not None:
            self._socket.close()
            self._socket = None

    def _receive_exactly(self, size: int) -> bytes:
        if self._socket is None:
            raise BoltStreamError("client is not connected")
        chunks = bytearray()
        while len(chunks) < size:
            chunk = self._socket.recv(size - len(chunks))
            if not chunk:
                raise ProtocolError("connection closed while receiving a frame")
            chunks.extend(chunk)
        return bytes(chunks)

    def _request(self, request_type: int, response_type: int, payload: bytes = b"") -> bytes:
        if self._socket is None:
            self.connect()
        assert self._socket is not None
        self._correlation_id += 1
        correlation_id = self._correlation_id
        self._socket.sendall(encode_frame(request_type, correlation_id, payload))
        header = self._receive_exactly(HEADER_BYTES)
        payload_bytes = struct.unpack(">I", header[12:16])[0]
        if payload_bytes > MAX_FRAME_BYTES - HEADER_BYTES:
            raise ProtocolError("response exceeds maximum frame size")
        frame_type, response = decode_frame(
            header + self._receive_exactly(payload_bytes), correlation_id
        )
        if frame_type == ERROR_RESPONSE:
            reader = _Reader(response)
            code, message = reader.u32(), reader.text()
            reader.finish()
            raise BrokerError(code, message)
        if frame_type != response_type:
            raise ProtocolError(f"expected frame type {response_type}, received {frame_type}")
        return response

    def health(self) -> dict[str, str]:
        reader = _Reader(self._request(HEALTH_REQUEST, HEALTH_RESPONSE))
        result = {"status": reader.text(), "detail": reader.text()}
        reader.finish()
        return result

    def create_topic(self, topic: str, partitions: int = 1) -> dict[str, object]:
        reader = _Reader(self._request(
            CREATE_TOPIC_REQUEST, CREATE_TOPIC_RESPONSE, _text(topic) + _u16(partitions)
        ))
        result = {"topic": reader.text(), "partitions": reader.u16(), "status": reader.text()}
        reader.finish()
        return result

    def produce(self, topic: str, key: str, message: str) -> dict[str, object]:
        payload = _text(topic) + _bytes(key.encode()) + _bytes(message.encode())
        reader = _Reader(self._request(PRODUCE_REQUEST, PRODUCE_RESPONSE, payload))
        result = {
            "topic": reader.text(), "partition": reader.u16(), "offset": reader.u64(),
            "next_offset": reader.u64(), "encoded_bytes": reader.u32(),
        }
        reader.finish()
        return result

    def fetch(self, topic: str, partition: int = 0, from_offset: str = "beginning",
              group: str = "", wait_ms: int = 0) -> dict[str, object]:
        payload = (_text(topic) + _u16(partition) + _text(from_offset) + _text(group)
                   + _u32(wait_ms))
        reader = _Reader(self._request(FETCH_REQUEST, FETCH_RESPONSE, payload))
        result: dict[str, object] = {
            "topic": reader.text(), "partition": reader.u16(),
            "from_offset": reader.u64(), "next_offset": reader.u64(),
        }
        records = []
        for _ in range(reader.u32()):
            records.append(Record(
                reader.u64(), reader.u64(), reader.text(), reader.text(), reader.u32()
            ))
        reader.finish()
        result["records"] = [asdict(record) for record in records]
        return result


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--token", default=os.getenv("BOLTSTREAM_BROKER_TOKEN"))
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("health")
    create = subcommands.add_parser("create-topic")
    create.add_argument("--topic", required=True)
    create.add_argument("--partitions", type=int, default=1)
    produce = subcommands.add_parser("produce")
    produce.add_argument("--topic", required=True)
    produce.add_argument("--key", default="")
    produce.add_argument("--message", required=True)
    fetch = subcommands.add_parser("fetch")
    fetch.add_argument("--topic", required=True)
    fetch.add_argument("--partition", type=int, default=0)
    fetch.add_argument("--from", dest="from_offset", default="beginning")
    fetch.add_argument("--group", default="")
    fetch.add_argument("--wait-ms", type=int, default=0)
    demo = subcommands.add_parser("demo")
    demo.add_argument("--topic", default="python-demo")
    demo.add_argument("--message", default="hello from Python")
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        with Client(args.host, args.port, args.token) as client:
            if args.command == "health":
                output = client.health()
            elif args.command == "create-topic":
                output = client.create_topic(args.topic, args.partitions)
            elif args.command == "produce":
                output = client.produce(args.topic, args.key, args.message)
            elif args.command == "fetch":
                output = client.fetch(args.topic, args.partition, args.from_offset,
                                      args.group, args.wait_ms)
            else:
                try:
                    created = client.create_topic(args.topic)
                except BrokerError as error:
                    if error.code != 12:
                        raise
                    created = {"topic": args.topic, "partitions": 1, "status": "exists"}
                produced = client.produce(args.topic, "python", args.message)
                fetched = client.fetch(args.topic, produced["partition"], str(produced["offset"]))
                output = {"created": created, "produced": produced, "fetched": fetched}
        print(json.dumps(output, separators=(",", ":")))
        return 0
    except (BoltStreamError, OSError) as error:
        print(json.dumps({"status": "error", "message": str(error)}, separators=(",", ":")),
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
