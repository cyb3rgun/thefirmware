#!/usr/bin/env python3
"""Run one S01 bench measurement and print the row for docs/measurements.md.

Drives both boards at once so that the radio numbers and the forwarding
numbers come from the same 100 shots rather than from two separate runs:

  module port    spoken to with the USB protocol of docs/usb-protocol.md.
                 The script is the core: it says hello, sets the beacons,
                 acknowledges every shot and asks for status at the end.
  stub port      read as plain text. The pistol stub keeps its own round
                 trip statistics and prints a summary when the run
                 completes; this script resets the board so the run starts
                 at zero and then waits for that summary.

Usage:
    python tools/bench_s01.py --module COM7 --stub COM6 --distance 3m
    python tools/bench_s01.py --module COM7 --stub COM6 --distance 1m --shots 100
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import cgusb_host as proto  # noqa: E402

SUMMARY_START = re.compile(r"-----\s*run summary\s*-----")
COUNTS = re.compile(r"sent\s+(\d+)\s+acked\s+(\d+)\s+lost\s+(\d+)\s+resends\s+(\d+)")
RTT = re.compile(
    r"rtt us:\s*median\s+(\d+)\s+p95\s+(\d+)\s+mean\s+(\d+)\s+min\s+(\d+)\s+max\s+(\d+)"
)
LOSS = re.compile(r"loss\s+([\d.]+)\s+percent,\s*rssi\s+(-?\d+)")


def reset_board(port):
    """Pulse EN through RTS, leaving GPIO0 high so the board runs the app."""
    port.setDTR(False)
    port.setRTS(True)
    time.sleep(0.12)
    port.setRTS(False)
    time.sleep(0.05)
    port.reset_input_buffer()


def open_port(serial_mod, name, baud):
    port = serial_mod.Serial(name, baud, timeout=0.02)
    port.setDTR(False)
    port.setRTS(False)
    return port


def wait_for_hello_ack(port, decoder, timeout=6.0):
    """The module never speaks first, so the session starts here."""
    deadline = time.monotonic() + timeout
    last_try = 0.0
    info = None

    while time.monotonic() < deadline:
        if time.monotonic() - last_try > 1.0:
            port.write(proto.encode("hello", proto=proto.PROTO_VERSION))
            last_try = time.monotonic()

        for item in decoder.feed(port.read(4096)):
            if item[0] == "frame" and item[1] == proto.BY_NAME["hello_ack"].type:
                info = item[2]
                return info
    return None


def main(argv: list[str]) -> int:
    try:
        import serial
    except ImportError:
        print("pyserial is needed. python -m pip install pyserial")
        return 2

    # The boards' log is whatever the firmware prints; a Windows console
    # at cp1252 would rather raise than show an odd byte.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--module", required=True, help="port of the target module")
    ap.add_argument("--stub", required=True, help="port of the pistol stub")
    ap.add_argument("--distance", required=True, help="label for the row, e.g. 3m")
    ap.add_argument("--shots", type=int, default=100)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--beacons", type=lambda s: int(s, 0), default=0x0F)
    ap.add_argument("--mode", type=int, default=0, choices=(0, 1))
    ap.add_argument("--period", type=int, default=40)
    ap.add_argument("--slot", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args(argv)

    module = open_port(serial, args.module, args.baud)
    stub = open_port(serial, args.stub, args.baud)
    decoder = proto.Decoder()

    print(f"=== S01 bench, {args.distance} ===")
    print(f"module {args.module}, stub {args.stub}, {args.shots} shots")

    # Hold the stub in reset, EN low, for the whole of the module handshake.
    # Without this the stub keeps firing from its previous run while the
    # module is booting, and the module hears shots before the core has said
    # hello. Those are counted as heard and correctly not forwarded, which
    # leaves shots heard one or two above the frames and makes the run look
    # lossy when it is not.
    stub.setDTR(False)
    stub.setRTS(True)

    # Bring the module up first and open the session, so that nothing the
    # stub fires is lost to a module that has not been told hello yet.
    print("holding the stub in reset, bringing the module up")
    reset_board(module)
    time.sleep(1.5)

    info = wait_for_hello_ack(module, decoder)
    if info is None:
        print("the module never answered hello. Is the port right?")
        return 1
    print(
        f"module fw {'.'.join(str(b) for b in info['fw'])} "
        f"mac {proto.mac_str(info['mac'])}"
    )

    module.write(
        proto.encode("beacons", mask=args.beacons, mode=args.mode,
                     period_ms=args.period, slot=args.slot)
    )
    print(f"beacons mask 0x{args.beacons:02X} mode {args.mode} slot {args.slot}")

    # Now let the stub go. Coming out of reset zeroes its counters, so the
    # first summary it prints covers exactly this distance.
    print("releasing the stub, the run starts now")
    stub.setRTS(False)
    time.sleep(0.05)
    stub.reset_input_buffer()

    frames = 0
    acked = 0
    bad = 0
    pistol_seqs = set()
    text = []
    summary: dict = {}
    deadline = time.monotonic() + args.timeout
    started = time.monotonic()

    while time.monotonic() < deadline:
        for item in decoder.feed(module.read(4096)):
            if item[0] == "frame":
                _, type_, fields = item
                if type_ == proto.BY_NAME["shot"].type:
                    frames += 1
                    pistol_seqs.add(fields["pistol_seq"])
                    module.write(proto.encode("ack", seq=fields["seq"]))
                    acked += 1
                elif type_ == proto.BY_NAME["status"].type:
                    summary["status"] = fields
                elif type_ == proto.BY_NAME["error"].type:
                    print(f"  module error: {proto.describe(type_, fields)}")
            elif item[0] == "bad":
                bad += 1

        chunk = stub.read(4096)
        if chunk:
            text.append(chunk.decode("utf-8", errors="replace"))
            joined = "".join(text)
            if SUMMARY_START.search(joined) and LOSS.search(joined):
                # The whole summary block has arrived.
                time.sleep(0.2)
                text.append(stub.read(4096).decode("utf-8", errors="replace"))
                break

    joined = "".join(text)
    elapsed = time.monotonic() - started

    counts = COUNTS.search(joined)
    rtt = RTT.search(joined)
    loss = LOSS.search(joined)

    if not (counts and rtt and loss):
        print("\nthe stub did not print a complete run summary inside the timeout")
        print("last text from the stub:")
        print("\n".join(joined.splitlines()[-15:]))
        return 1

    sent, stub_acked, lost, resends = (int(x) for x in counts.groups())
    median, p95, mean, rtt_min, rtt_max = (int(x) for x in rtt.groups())
    loss_pct, rssi = float(loss.group(1)), int(loss.group(2))

    # Ask the module what it counted, now that the run is over.
    module.write(proto.encode("status_req"))
    until = time.monotonic() + 2.0
    while time.monotonic() < until:
        for item in decoder.feed(module.read(4096)):
            if item[0] == "frame" and item[1] == proto.BY_NAME["status"].type:
                summary["status"] = item[2]
                until = 0
                break

    status = summary.get("status", {})

    print(f"\nrun took {elapsed:.1f} s")
    print("\n--- section 1, shot to acknowledgement round trip ---")
    print(f"| {args.distance} | {sent} | {median} | {p95} | "
          f"{loss_pct:.2f} percent | {resends} | {rssi} |")
    print(f"  mean {mean} us, min {rtt_min} us, max {rtt_max} us, "
          f"stub acked {stub_acked}, lost {lost}")

    print("\n--- section 2, forwarded frames against heard shots ---")
    heard = status.get("shots_heard", "?")
    core_acked = status.get("shots_acked", "?")
    distinct = len(pistol_seqs)
    # Shots the module heard but did not turn into a frame. Duplicates from a
    # pistol that missed an acknowledgement land here, and so would anything
    # heard before the core said hello.
    unforwarded = "?" if heard == "?" else max(0, int(heard) - frames)
    print(f"| {args.distance} | {sent} | {heard} | {frames} | {core_acked} | "
          f"{unforwarded} |")
    print(f"  distinct pistol sequence numbers in the forwarded frames: {distinct}")
    print(f"  frames the host could not decode: {bad}")
    if status:
        print(f"  module status: {proto.describe(proto.BY_NAME['status'].type, status)}")

    ok = (frames == distinct) and (lost == 0)
    print(f"\nzero loss and one frame per distinct shot: {'yes' if ok else 'NO'}")

    module.close()
    stub.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
