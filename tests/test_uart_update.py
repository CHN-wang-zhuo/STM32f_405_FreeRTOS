from pathlib import Path
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import uart_update as protocol


class ProtocolTest(unittest.TestCase):
    def test_wire_and_fragmented_reply(self):
        payload = struct.pack("<III", 4096, 0x12345678, 7)
        wire = protocol.encode(protocol.BEGIN, 42, payload)
        self.assertEqual(wire[:12], b"FWUP\x01\x02\x0c\x00\x2a\x00\x00\x00")
        self.assertEqual(struct.unpack_from("<I", wire, 12)[0], zlib.crc32(wire[4:12] + payload))
        decoder = protocol.Decoder()
        frames = []
        for byte in b"noiseFFF" + wire:
            frames.extend(decoder.feed(bytes([byte])))
        self.assertEqual(frames, [(protocol.BEGIN, 42, payload)])

    def test_corruption_and_size(self):
        decoder = protocol.Decoder()
        wire = bytearray(protocol.encode(protocol.QUERY, 1))
        wire[12] ^= 1
        self.assertEqual(decoder.feed(wire), [])
        good = protocol.encode(protocol.QUERY, 2)
        self.assertEqual(decoder.feed(good), [(protocol.QUERY, 2, b"")])
        with self.assertRaises(ValueError):
            protocol.encode(protocol.DATA, 3, bytes(1029))

    def test_client_retries_identical_frame(self):
        class Serial:
            def __init__(self): self.writes = []
            def write(self, data): self.writes.append(data)
            def read(self, _):
                if len(self.writes) < 2: return b""
                command, sequence = self.writes[-1][5], struct.unpack_from("<I", self.writes[-1], 8)[0]
                payload = struct.pack("<4B6I", command, 0, 0, 0, 0, 0, 0, 0, 1024, 1)
                return protocol.encode(command | 0x80, sequence, payload)
        serial = Serial()
        result = protocol.Client(serial, timeout=0.01).request(protocol.QUERY)
        self.assertEqual(serial.writes[0], serial.writes[1])
        self.assertEqual(result["chunk"], 1024)


if __name__ == "__main__": unittest.main()
