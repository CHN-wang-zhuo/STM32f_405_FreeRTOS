"""USART1 stop-and-wait firmware updater; Windows needs no additional packages."""
import argparse
from pathlib import Path
import secrets
import struct
import time
import zlib

from serial_link import open_serial

MAGIC = b"FWUP"
QUERY, BEGIN, DATA, END, ABORT, REBOOT = range(1, 7)
HEADER = struct.Struct("<4sBBHII")
MAX_PAYLOAD = 1028
ERRORS = ["OK", "BAD_COMMAND", "BAD_LENGTH", "BAD_STATE", "BAD_OFFSET",
          "BAD_CRC", "FLASH_ERROR", "BAD_IMAGE", "BAD_SEQUENCE"]


def encode(command, sequence, payload=b""):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("Payload exceeds 1028 bytes")
    prefix = struct.pack("<BBHI", 1, command, len(payload), sequence)
    crc = zlib.crc32(prefix + payload)
    return MAGIC + prefix + struct.pack("<I", crc) + payload


class Decoder:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        frames = []
        while True:
            position = self.buffer.find(MAGIC)
            if position < 0:
                self.buffer[:] = self.buffer[-3:]
                break
            del self.buffer[:position]
            if len(self.buffer) < HEADER.size:
                break
            _, version, command, length, sequence, crc = HEADER.unpack_from(self.buffer)
            if version != 1 or length > MAX_PAYLOAD:
                del self.buffer[0]
                continue
            if len(self.buffer) < HEADER.size + length:
                break
            payload = bytes(self.buffer[HEADER.size:HEADER.size + length])
            valid = zlib.crc32(bytes(self.buffer[4:12]) + payload) == crc
            del self.buffer[:HEADER.size + length]
            if valid:
                frames.append((command, sequence, payload))
        return frames


class Client:
    def __init__(self, serial, timeout=20, retries=3):
        self.serial = serial
        self.timeout = timeout
        self.retries = retries
        self.sequence = secrets.randbits(32)
        self.decoder = Decoder()

    def request(self, command, payload=b""):
        self.sequence = (self.sequence + 1) & 0xFFFFFFFF
        wire = encode(command, self.sequence, payload)
        for _ in range(self.retries):
            self.serial.write(wire)
            deadline = time.monotonic() + self.timeout
            while time.monotonic() < deadline:
                for response_command, sequence, body in self.decoder.feed(self.serial.read(4096)):
                    if response_command != command | 0x80 or sequence != self.sequence or len(body) != 28:
                        continue
                    original, status, state, reset, received, size, version, crc, chunk, running = struct.unpack("<4B6I", body)
                    if original != command:
                        continue
                    result = dict(status=status, state=state, reset=reset, received=received,
                                  size=size, version=version, crc=crc, chunk=chunk, running_version=running)
                    if status:
                        message = ERRORS[status] if status < len(ERRORS) else str(status)
                        raise RuntimeError(f"Device rejected command {command}: {message}, {result}")
                    return result
        raise TimeoutError(f"No valid ACK for command {command}, sequence {self.sequence}; query device before restarting")

    def upload(self, image, version, mode):
        validate_image(image)
        result = self.request(BEGIN, struct.pack("<III", len(image), zlib.crc32(image), version))
        chunk = min(result["chunk"], 1024)
        if chunk < 4 or chunk & 3:
            raise RuntimeError("Invalid device chunk size")
        for offset in range(0, len(image), chunk):
            block = image[offset:offset + chunk]
            result = self.request(DATA, struct.pack("<I", offset) + block)
            if result["received"] != offset + len(block):
                raise RuntimeError(f"Unexpected device progress: {result}")
            print(f"\rUploaded {result['received']}/{len(image)}", end="", flush=True)
        print()
        return self.request(END, bytes([1 if mode == "now" else 0]))


def validate_image(image):
    if not 8 <= len(image) <= 0x60000:
        raise ValueError("APP size must be 8..393216 bytes")
    stack, reset = struct.unpack_from("<II", image)
    if stack & 7 or not (0x20000000 < stack <= 0x20020000 or 0x10000000 < stack <= 0x10010000):
        raise ValueError("Invalid APP initial stack pointer")
    if not reset & 1 or not 0x08020008 <= (reset & ~1) < 0x08020000 + len(image):
        raise ValueError("APP must be linked for 0x08020000, with a Thumb Reset_Handler")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=20)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("query")
    upload = commands.add_parser("upload")
    upload.add_argument("image", type=Path)
    upload.add_argument("--version", type=lambda x: int(x, 0), required=True)
    upload.add_argument("--mode", choices=["defer", "now"], required=True)
    commands.add_parser("abort")
    commands.add_parser("reboot")
    args = parser.parse_args()
    image = None
    if args.command == "upload":
        image = args.image.read_bytes()
        validate_image(image)
        if not 0 <= args.version <= 0xFFFFFFFF:
            parser.error("version must fit uint32")
    with open_serial(args.port, args.baud) as serial:
        client = Client(serial, timeout=args.timeout)
        if args.command == "upload":
            result = client.upload(image, args.version, args.mode)
        else:
            result = client.request({"query": QUERY, "abort": ABORT, "reboot": REBOOT}[args.command])
        print(result)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        raise SystemExit(str(error)) from None
