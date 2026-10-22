"""Explicit W25Q16 hardware test; requires this external-storage APP installed first.

Writes the external candidate partition and requests a reset. The unchanged
Bootloader must keep running the old APP; this test does not verify installation.
"""
import argparse
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from serial_link import open_serial
from uart_update import Client, QUERY, BEGIN, DATA, END, encode, validate_image


def run(port, image, version):
    # Exercise the final partial Flash word without changing any executable byte.
    image += b"\xFF" * ((3 - len(image)) % 4)
    validate_image(image)
    with open_serial(port) as serial:
        client = Client(serial)
        before = client.request(QUERY)
        print("Before:", before)
        corrupt = bytearray(encode(QUERY, 0x12345678))
        corrupt[12] ^= 1
        serial.write(b"garbageFFF" + corrupt)
        assert client.request(QUERY)["running_version"] == before["running_version"]
        begin = struct.pack("<III", len(image), zlib.crc32(image), version)
        first = client.request(BEGIN, begin)
        client.sequence = (client.sequence - 1) & 0xFFFFFFFF
        assert client.request(BEGIN, begin) == first
        print("PASS bad frame ignored, noise resynchronized, duplicate BEGIN accepted")
        for offset in range(0, len(image), 1024):
            payload = struct.pack("<I", offset) + image[offset:offset + 1024]
            result = client.request(DATA, payload)
            assert result["received"] == min(offset + 1024, len(image))
            if offset == 0:
                client.sequence = (client.sequence - 1) & 0xFFFFFFFF
                assert client.request(DATA, payload) == result
                assert client.request(DATA, payload)["received"] == result["received"]
        try:
            client.request(END, b"\x02")
        except RuntimeError as error:
            assert "BAD_LENGTH" in str(error)
        else:
            raise AssertionError("Invalid update mode was accepted")
        result = client.request(END, b"\x01")
        assert result["reset"] == 1 and result["version"] == version
        print("PASS duplicate DATA and partial final block; invalid mode rejected; reset ACK:", result)
    with open_serial(port) as serial:
        after = Client(serial).request(QUERY)
        assert after["state"] == 2 and after["version"] == version
        assert after["size"] == len(image) and after["crc"] == zlib.crc32(image)
        assert after["running_version"] == before["running_version"]
        print("PASS external candidate recovered after reset; original running version unchanged:", after)
    return image


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--version", type=int, required=True)
    parser.add_argument("--save-image", type=Path, required=True)
    args = parser.parse_args()
    uploaded = run(args.port, args.image.read_bytes(), args.version)
    args.save_image.write_bytes(uploaded)
