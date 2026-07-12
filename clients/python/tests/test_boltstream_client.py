import importlib.util
import pathlib
import struct
import sys
import unittest


MODULE_PATH = pathlib.Path(__file__).parents[1] / "boltstream_client.py"
SPEC = importlib.util.spec_from_file_location("boltstream_client", MODULE_PATH)
client = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = client
SPEC.loader.exec_module(client)


class FrameTests(unittest.TestCase):
    def test_frame_round_trip(self):
        encoded = client.encode_frame(client.HEALTH_REQUEST, 42, b"payload")
        frame_type, payload = client.decode_frame(encoded, 42)
        self.assertEqual(client.HEALTH_REQUEST, frame_type)
        self.assertEqual(b"payload", payload)

    def test_crc_corruption_is_rejected(self):
        encoded = bytearray(client.encode_frame(client.HEALTH_REQUEST, 1))
        encoded[7] ^= 0x01
        with self.assertRaisesRegex(client.ProtocolError, "CRC"):
            client.decode_frame(bytes(encoded), 1)

    def test_wrong_correlation_is_rejected(self):
        encoded = client.encode_frame(client.HEALTH_RESPONSE, 8)
        with self.assertRaisesRegex(client.ProtocolError, "correlation"):
            client.decode_frame(encoded, 9)


class ReaderTests(unittest.TestCase):
    def test_fetch_payload_decodes_all_fields(self):
        payload = (
            client._text("events") + client._u16(0) + struct.pack(">QQI", 4, 5, 1)
            + struct.pack(">QQ", 4, 123) + client._text("key") + client._text("value")
            + client._u32(37)
        )
        reader = client._Reader(payload)
        self.assertEqual("events", reader.text())
        self.assertEqual(0, reader.u16())
        self.assertEqual(4, reader.u64())
        self.assertEqual(5, reader.u64())
        self.assertEqual(1, reader.u32())
        self.assertEqual(4, reader.u64())
        self.assertEqual(123, reader.u64())
        self.assertEqual("key", reader.text())
        self.assertEqual("value", reader.text())
        self.assertEqual(37, reader.u32())
        reader.finish()

    def test_trailing_payload_is_rejected(self):
        reader = client._Reader(client._text("ok") + b"x")
        self.assertEqual("ok", reader.text())
        with self.assertRaisesRegex(client.ProtocolError, "trailing"):
            reader.finish()


class BrokerErrorTests(unittest.TestCase):
    def test_broker_error_exposes_code_and_message(self):
        error = client.BrokerError(12, "topic exists")
        self.assertEqual(12, error.code)
        self.assertEqual("topic exists", error.message)
        self.assertIn("broker error 12", str(error))

    def test_request_raises_structured_broker_error(self):
        response = client.encode_frame(
            client.ERROR_RESPONSE, 1, client._u32(11) + client._text("unknown topic")
        )

        class FakeSocket:
            def __init__(self, data):
                self.data = bytearray(data)

            def sendall(self, _data):
                pass

            def recv(self, size):
                chunk = self.data[:size]
                del self.data[:size]
                return bytes(chunk)

        connection = client.Client()
        connection._socket = FakeSocket(response)
        with self.assertRaises(client.BrokerError) as caught:
            connection._request(client.HEALTH_REQUEST, client.HEALTH_RESPONSE)
        self.assertEqual(11, caught.exception.code)
        self.assertEqual("unknown topic", caught.exception.message)


if __name__ == "__main__":
    unittest.main()
