#!/usr/bin/env python3
"""Host side reference implementation of docs/usb-protocol.md, version 1.

Two jobs:

1. It is the reference the firmware is tested against. `vectors` prints test
   vectors as JSON; `test/host/test_cgusb.c` checks that the C encoder in
   `components/cgusb` produces exactly the same bytes. The two sides are then
   proven to agree, not merely written from the same document.
2. It is the bench tool. `monitor` opens the module's serial port, says
   `hello`, decodes what comes back and acknowledges shots, so a bench runs
   before theclient B04 exists.

The CRC variant is D-005 and is still open with the architect. Change
DEFAULT_CRC below and the matching constant in components/cgusb/cgusb.c and
both sides move together.

Usage:
    python tools/cgusb_host.py selftest
    python tools/cgusb_host.py vectors
    python tools/cgusb_host.py decode 01 04 01 ...
    python tools/cgusb_host.py monitor COM6
    python tools/cgusb_host.py monitor COM6 --beacons 0x0F --mode 1 --period 40 --slot 1
"""
from __future__ import annotations

import argparse
import binascii
import json
import struct
import sys
import time
from dataclasses import dataclass, field

PROTO_VERSION = 1

# --------------------------------------------------------------------- crc --

# name: (polynomial, initial value, input reflected, output reflected, xor out)
CRC_VARIANTS = {
    # D-005: what thefirmware implements today.
    "ccitt-false": (0x1021, 0xFFFF, False, False, 0x0000),
    # The other two readings of "CRC-16/CCITT", kept so that a different
    # ruling from the architect costs one line on each side.
    "kermit": (0x1021, 0x0000, True, True, 0x0000),
    "xmodem": (0x1021, 0x0000, False, False, 0x0000),
}
DEFAULT_CRC = "ccitt-false"

# The value every variant must produce for the string "123456789".
CRC_CHECK_VALUES = {"ccitt-false": 0x29B1, "kermit": 0x2189, "xmodem": 0x31C3}


def _reflect(value: int, width: int) -> int:
    out = 0
    for _ in range(width):
        out = (out << 1) | (value & 1)
        value >>= 1
    return out


def crc16(data: bytes, variant: str = DEFAULT_CRC) -> int:
    poly, init, ref_in, ref_out, xor_out = CRC_VARIANTS[variant]
    crc = init
    for byte in data:
        if ref_in:
            byte = _reflect(byte, 8)
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    if ref_out:
        crc = _reflect(crc, 16)
    return crc ^ xor_out


# -------------------------------------------------------------------- cobs --


def cobs_encode(data: bytes) -> bytes:
    """Encode one block. The trailing zero delimiter is not added here."""
    out = bytearray()
    block = bytearray()
    for byte in data:
        if byte == 0:
            out.append(len(block) + 1)
            out.extend(block)
            block.clear()
        else:
            block.append(byte)
            if len(block) == 254:
                out.append(0xFF)
                out.extend(block)
                block.clear()
    out.append(len(block) + 1)
    out.extend(block)
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    """Decode one block that carries no zero byte. Raises on a bad block."""
    out = bytearray()
    index = 0
    n = len(data)
    while index < n:
        code = data[index]
        if code == 0:
            raise ValueError("zero byte inside a COBS block")
        if index + code > n:
            raise ValueError("COBS code runs past the end of the block")
        out.extend(data[index + 1 : index + code])
        index += code
        if code != 0xFF and index < n:
            out.append(0)
    return bytes(out)


# ---------------------------------------------------------------- messages --


@dataclass(frozen=True)
class Message:
    type: int
    name: str
    fmt: str
    fields: tuple[str, ...]

    @property
    def size(self) -> int:
        return struct.calcsize(self.fmt) if self.fmt else 0


# usb-protocol.md sections 2 and 3. Little endian, packed, no alignment.
MESSAGES: tuple[Message, ...] = (
    # core to module
    Message(0x01, "hello", "<B", ("proto",)),
    Message(0x02, "beacons", "<BBHB", ("mask", "mode", "period_ms", "slot")),
    Message(0x03, "time_mark", "<Q", ("unix_ms",)),
    Message(0x04, "ack", "<H", ("seq",)),
    Message(0x05, "channel", "<B", ("channel",)),
    Message(0x06, "status_req", "", ()),
    Message(0x07, "reboot", "", ()),
    # module to core
    Message(0x81, "hello_ack", "<B3sB6s", ("proto", "fw", "chip", "mac")),
    Message(
        0x82,
        "shot",
        "<H6sHBBIHHhhhhBb",
        (
            "seq", "pistol_id", "pistol_seq", "slot", "flags", "ts_pistol_ms",
            "x", "y", "q0", "q1", "q2", "q3", "buttons", "rssi",
        ),
    ),
    Message(
        0x83,
        "status",
        "<IBBBbIII",
        (
            "uptime_s", "channel", "beacons_mask", "mode", "temp",
            "free_heap", "shots_heard", "shots_acked",
        ),
    ),
    Message(0x84, "pistol_seen", "<6sb", ("pistol_id", "rssi")),
    # error is the one variable message: code plus 0 to 32 detail bytes.
    Message(0x85, "error", "", ("code", "detail")),
)

BY_TYPE = {m.type: m for m in MESSAGES}
BY_NAME = {m.name: m for m in MESSAGES}

ERROR_DETAIL_MAX = 32
BEACON_MODE_STEADY = 0
BEACON_MODE_MULTIPLEX = 1
SHOT_FLAG_UNAIMED = 1 << 0

CHIP_NAMES = {1: "esp32", 2: "esp32s3", 3: "esp32p4"}
ERROR_NAMES = {
    1: "bad_crc",
    2: "bad_frame",
    3: "unknown_type",
    4: "bad_length",
    5: "proto_version",
    6: "shot_dropped",
    7: "radio",
}


def pack_payload(name: str, **fields) -> bytes:
    msg = BY_NAME[name]
    if name == "error":
        detail = fields.get("detail", b"")
        if len(detail) > ERROR_DETAIL_MAX:
            raise ValueError(f"error detail is {len(detail)} bytes, at most {ERROR_DETAIL_MAX}")
        return bytes([fields["code"]]) + bytes(detail)
    if not msg.fmt:
        return b""
    return struct.pack(msg.fmt, *(fields[f] for f in msg.fields))


def unpack_payload(type_: int, payload: bytes) -> dict:
    msg = BY_TYPE[type_]
    if msg.name == "error":
        if not payload:
            raise ValueError("error needs at least a code byte")
        return {"code": payload[0], "detail": payload[1:]}
    if not msg.fmt:
        if payload:
            raise ValueError(f"{msg.name} takes no payload, got {len(payload)} bytes")
        return {}
    if len(payload) != msg.size:
        raise ValueError(f"{msg.name} wants {msg.size} payload bytes, got {len(payload)}")
    return dict(zip(msg.fields, struct.unpack(msg.fmt, payload)))


# ------------------------------------------------------------------ frames --


def encode_frame(type_: int, payload: bytes = b"", variant: str = DEFAULT_CRC) -> bytes:
    """type, payload, CRC little endian, COBS encoded, zero delimited.

    A delimiter is written before the frame as well as after it (D-008). It
    closes off whatever came before, so log text sharing the port cannot
    merge into the frame's COBS block and destroy the frame.
    """
    body = bytes([type_]) + payload
    crc = crc16(body, variant)
    return b"\x00" + cobs_encode(body + struct.pack("<H", crc)) + b"\x00"


def encode(name: str, variant: str = DEFAULT_CRC, **fields) -> bytes:
    return encode_frame(BY_NAME[name].type, pack_payload(name, **fields), variant)


def decode_block(block: bytes, variant: str = DEFAULT_CRC) -> tuple[int, bytes]:
    """Take one COBS block without its delimiter, return type and payload."""
    raw = cobs_decode(block)
    if len(raw) < 3:
        raise ValueError(f"frame is {len(raw)} bytes, the shortest legal frame is 3")
    body, crc_bytes = raw[:-2], raw[-2:]
    want = struct.unpack("<H", crc_bytes)[0]
    got = crc16(body, variant)
    if got != want:
        raise ValueError(f"CRC is 0x{got:04X}, the frame says 0x{want:04X}")
    type_ = body[0]
    if type_ not in BY_TYPE:
        raise ValueError(f"type 0x{type_:02X} is not part of version 1")
    return type_, body[1:]


@dataclass
class Decoder:
    """Byte at a time receiver, the mirror of cgusb_rx_t.

    Bytes that are not part of a frame are handed back as text, which is how
    the ESP-IDF log that shares UART0 stays readable at the bench (D-006).
    """

    variant: str = DEFAULT_CRC
    buf: bytearray = field(default_factory=bytearray)
    frames_ok: int = 0
    frames_bad: int = 0

    def feed(self, data: bytes):
        """Yield ('frame', type, fields) and ('text', line) as they complete."""
        for byte in data:
            if byte != 0:
                self.buf.append(byte)
                continue
            block, self.buf = bytes(self.buf), bytearray()
            if not block:
                continue
            try:
                type_, payload = decode_block(block, self.variant)
                fields = unpack_payload(type_, payload)
            except ValueError as exc:
                self.frames_bad += 1
                # Printable runs are log output, not a broken frame.
                text = block.decode("utf-8", errors="replace").strip()
                if text and sum(32 <= c < 127 or c in (9, 10, 13) for c in block) > len(block) * 0.8:
                    for line in text.splitlines():
                        if line.strip():
                            yield ("text", line.rstrip())
                else:
                    yield ("bad", f"{exc} in {block.hex(' ')}")
                continue
            self.frames_ok += 1
            yield ("frame", type_, fields)


# ------------------------------------------------------------------ pretty --


def mac_str(raw: bytes) -> str:
    return ":".join(f"{b:02X}" for b in raw)


def describe(type_: int, fields: dict) -> str:
    name = BY_TYPE[type_].name
    if name == "hello_ack":
        fw = ".".join(str(b) for b in fields["fw"])
        chip = CHIP_NAMES.get(fields["chip"], f"chip{fields['chip']}")
        return f"hello_ack proto={fields['proto']} fw={fw} chip={chip} mac={mac_str(fields['mac'])}"
    if name == "shot":
        flags = "unaimed" if fields["flags"] & SHOT_FLAG_UNAIMED else "aimed"
        return (
            f"shot seq={fields['seq']} pistol={mac_str(fields['pistol_id'])} "
            f"pseq={fields['pistol_seq']} slot={fields['slot']} {flags} "
            f"x={fields['x']} y={fields['y']} ts={fields['ts_pistol_ms']}ms "
            f"rssi={fields['rssi']}"
        )
    if name == "status":
        return (
            f"status up={fields['uptime_s']}s ch={fields['channel']} "
            f"beacons=0x{fields['beacons_mask']:02X} mode={fields['mode']} "
            f"temp={fields['temp']}C heap={fields['free_heap']} "
            f"heard={fields['shots_heard']} acked={fields['shots_acked']}"
        )
    if name == "pistol_seen":
        return f"pistol_seen {mac_str(fields['pistol_id'])} rssi={fields['rssi']}"
    if name == "error":
        code = ERROR_NAMES.get(fields["code"], str(fields["code"]))
        detail = fields["detail"].hex(" ") if fields["detail"] else ""
        return f"error {code} {detail}".rstrip()
    return f"{name} {fields}"


# --------------------------------------------------------------- selftest ---


def build_vectors() -> list[dict]:
    """Frames the C side must reproduce byte for byte. Values are arbitrary
    but fixed, and chosen so every field has a distinct, asymmetric value."""
    mac = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11])
    pistol = bytes([0x24, 0x6F, 0x28, 0x01, 0x02, 0x03])
    cases = [
        ("hello", dict(proto=1)),
        ("beacons", dict(mask=0x0F, mode=1, period_ms=40, slot=3)),
        ("time_mark", dict(unix_ms=1758326400123)),
        ("ack", dict(seq=0x1234)),
        ("channel", dict(channel=6)),
        ("status_req", dict()),
        ("reboot", dict()),
        ("hello_ack", dict(proto=1, fw=bytes([0, 1, 0]), chip=1, mac=mac)),
        (
            "shot",
            dict(
                seq=0x0102, pistol_id=pistol, pistol_seq=0x0304, slot=2, flags=1,
                ts_pistol_ms=0x05060708, x=0x090A, y=0x0B0C,
                q0=1000, q1=-2000, q2=3000, q3=-4000, buttons=0x05, rssi=-42,
            ),
        ),
        (
            "status",
            dict(
                uptime_s=123456, channel=6, beacons_mask=0x0F, mode=1, temp=-7,
                free_heap=200000, shots_heard=1000, shots_acked=999,
            ),
        ),
        ("pistol_seen", dict(pistol_id=pistol, rssi=-60)),
        ("error", dict(code=2, detail=b"")),
        ("error", dict(code=6, detail=bytes(range(32)))),
    ]
    # A payload full of zero bytes is the case COBS exists for, so it gets
    # its own vector.
    cases.append(("time_mark", dict(unix_ms=0)))

    out = []
    for name, fields in cases:
        payload = pack_payload(name, **fields)
        frame = encode(name, **fields)
        out.append(
            {
                "name": name,
                "type": BY_NAME[name].type,
                "payload": payload.hex(),
                "frame": frame.hex(),
                "crc": crc16(bytes([BY_NAME[name].type]) + payload),
            }
        )
    return out


def selftest() -> int:
    failures = []

    for variant, want in CRC_CHECK_VALUES.items():
        got = crc16(b"123456789", variant)
        if got != want:
            failures.append(f"crc {variant}: got 0x{got:04X}, want 0x{want:04X}")

    # COBS round trip, including the cases the algorithm exists for.
    probes = [
        b"", b"\x00", b"\x00\x00", b"\x01\x02\x03", b"\x11\x00\x22\x00\x33",
        bytes(range(1, 255)), bytes(254), bytes([1] * 300), bytes(300),
    ]
    for probe in probes:
        encoded = cobs_encode(probe)
        if 0 in encoded:
            failures.append(f"cobs encode left a zero byte in {probe.hex()[:32]}")
        back = cobs_decode(encoded)
        if back != probe:
            failures.append(f"cobs round trip failed for {probe.hex()[:32]}")

    # Every message round trips through a full frame.
    for vector in build_vectors():
        frame = bytes.fromhex(vector["frame"])
        if frame[0] != 0 or frame[-1] != 0:
            failures.append(f"{vector['name']}: frame is not delimited on both sides")
        if 0 in frame[1:-1]:
            failures.append(f"{vector['name']}: zero byte inside the frame body")
        type_, payload = decode_block(frame[1:-1])
        if type_ != vector["type"]:
            failures.append(f"{vector['name']}: type came back as 0x{type_:02X}")
        if payload.hex() != vector["payload"]:
            failures.append(f"{vector['name']}: payload did not round trip")
        unpack_payload(type_, payload)

    # A flipped bit must be caught by the CRC, not passed on.
    frame = bytearray(encode("ack", seq=1))
    frame[2] ^= 0x01
    try:
        decode_block(bytes(frame[1:-1]))
        failures.append("a flipped bit passed the CRC")
    except ValueError:
        pass

    # The streaming decoder finds frames in a stream that also carries log
    # text and a truncated frame, which is the bench case of D-006.
    stream = (
        b"I (123) boot: hello from the module\r\n"
        + encode("status_req")
        + encode("ack", seq=7)
        + b"\x05\x06"  # a truncated frame, no delimiter yet
        + b"\x00"
        + encode("channel", channel=11)
    )
    decoder = Decoder()
    seen = [item for item in decoder.feed(stream)]
    frames = [item for item in seen if item[0] == "frame"]
    if len(frames) != 3:
        failures.append(f"decoder found {len(frames)} frames in the mixed stream, want 3")
    if not any(item[0] == "text" for item in seen):
        failures.append("decoder swallowed the log text instead of passing it through")

    if failures:
        print(f"cgusb_host selftest: {len(failures)} failure(s)")
        for line in failures:
            print(f"  {line}")
        return 1
    print("cgusb_host selftest: ok")
    return 0


# ---------------------------------------------------------------- monitor ---


def monitor(args) -> int:
    try:
        import serial  # noqa: PLC0415
    except ImportError:
        print("pyserial is needed for monitor. Install it with:")
        print("    python -m pip install pyserial")
        return 2

    shots = 0
    started = time.monotonic()

    with serial.Serial(args.port, args.baud, timeout=0.05) as port:
        print(f"open {args.port} at {args.baud}, saying hello")
        port.write(encode("hello", proto=PROTO_VERSION))

        if args.beacons is not None:
            port.write(
                encode("beacons", mask=args.beacons, mode=args.mode,
                       period_ms=args.period, slot=args.slot)
            )
            print(f"beacons mask=0x{args.beacons:02X} mode={args.mode} "
                  f"period={args.period}ms slot={args.slot}")
        if args.channel is not None:
            port.write(encode("channel", channel=args.channel))
            print(f"channel {args.channel}")

        decoder = Decoder()
        last_status_req = time.monotonic()
        try:
            while True:
                chunk = port.read(4096)
                if chunk:
                    for item in decoder.feed(chunk):
                        stamp = f"{time.monotonic() - started:9.3f}"
                        if item[0] == "frame":
                            _, type_, fields = item
                            print(f"{stamp}  {describe(type_, fields)}")
                            if type_ == BY_NAME["shot"].type:
                                shots += 1
                                # usb-protocol.md section 4: the core's ack
                                # frees the module's forwarding buffer.
                                port.write(encode("ack", seq=fields["seq"]))
                        elif item[0] == "text":
                            print(f"{stamp}  . {item[1]}")
                        else:
                            print(f"{stamp}  ! {item[1]}")
                now = time.monotonic()
                if args.poll and now - last_status_req >= args.poll:
                    port.write(encode("status_req"))
                    last_status_req = now
        except KeyboardInterrupt:
            print(
                f"\nstopped. frames ok {decoder.frames_ok}, bad {decoder.frames_bad}, "
                f"shots acked {shots}"
            )
    return 0


# ------------------------------------------------------------------- main ---


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("selftest", help="check the codec against its own vectors")
    sub.add_parser("vectors", help="print the test vectors as JSON")

    p_decode = sub.add_parser("decode", help="decode one frame given as hex bytes")
    p_decode.add_argument("hex", nargs="+")

    p_mon = sub.add_parser("monitor", help="open a module's port and follow it")
    p_mon.add_argument("port")
    p_mon.add_argument("--baud", type=int, default=921600)
    p_mon.add_argument("--beacons", type=lambda s: int(s, 0), default=None,
                       help="cluster mask, for example 0x0F")
    p_mon.add_argument("--mode", type=int, default=BEACON_MODE_STEADY, choices=(0, 1))
    p_mon.add_argument("--period", type=int, default=40, help="multiplex period in ms")
    p_mon.add_argument("--slot", type=int, default=1)
    p_mon.add_argument("--channel", type=int, default=None)
    p_mon.add_argument("--poll", type=float, default=0.0,
                       help="seconds between status_req, 0 to never ask")

    args = parser.parse_args(argv)

    if args.cmd == "selftest":
        return selftest()
    if args.cmd == "vectors":
        print(json.dumps(build_vectors(), indent=2))
        return 0
    if args.cmd == "decode":
        raw = binascii.unhexlify("".join(args.hex).replace(" ", ""))
        block = raw.strip(b"\x00")
        type_, payload = decode_block(block)
        print(describe(type_, unpack_payload(type_, payload)))
        return 0
    return monitor(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
