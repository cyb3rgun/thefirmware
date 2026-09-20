#!/usr/bin/env python3
"""Run one S01 bench measurement and print the rows for docs/measurements.md.

Everything goes through the module's port. The pistol needs no cable to the
PC and can sit on a power bank wherever the distance says (D-014):

  1. The script is the core. It says hello, sets the beacons, and sends
     bench_start with the shot count and the rate.
  2. The module broadcasts a start packet. The pistol runs exactly that many
     shots, keeping its own round trip statistics.
  3. The pistol sends one report back over the radio. The module prints it
     as a line and forwards it as a bench_report frame.
  4. Meanwhile this script acknowledges every forwarded shot, so the
     forwarding path is exercised and counted at the same time.

Usage:
    python tools/bench_s01.py --module COM7 --distance "1 m"
    python tools/bench_s01.py --module COM7 --distance "5 m" --shots 100 --rate 200
"""
from __future__ import annotations

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import cgusb_host as proto  # noqa: E402


def reset_board(port):
    """Pulse EN through RTS, leaving GPIO0 high so the board runs the app."""
    port.setDTR(False)
    port.setRTS(True)
    time.sleep(0.12)
    port.setRTS(False)
    time.sleep(0.05)
    port.reset_input_buffer()


def wait_for_hello_ack(port, decoder, timeout=8.0):
    """The module never speaks first, so the session starts here."""
    deadline = time.monotonic() + timeout
    last_try = 0.0

    while time.monotonic() < deadline:
        if time.monotonic() - last_try > 1.0:
            port.write(proto.encode("hello", proto=proto.PROTO_VERSION))
            last_try = time.monotonic()
        for item in decoder.feed(port.read(4096)):
            if item[0] == "frame" and item[1] == proto.BY_NAME["hello_ack"].type:
                return item[2]
    return None


def main(argv: list[str]) -> int:
    try:
        import serial
    except ImportError:
        print("pyserial is needed. python -m pip install pyserial")
        return 2

    # The board's log is whatever the firmware prints; a Windows console at
    # cp1252 would rather raise than show an odd byte.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--module", required=True, help="port of the target module")
    ap.add_argument("--distance", required=True, help="label for the row, e.g. 3 m")
    ap.add_argument("--shots", type=int, default=100)
    ap.add_argument("--rate", type=int, default=200, help="ms between shots")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--beacons", type=lambda s: int(s, 0), default=0x0F)
    ap.add_argument("--mode", type=int, default=0, choices=(0, 1))
    ap.add_argument("--period", type=int, default=40)
    ap.add_argument("--slot", type=int, default=1)
    ap.add_argument("--verbose", action="store_true", help="show the module's log")
    args = ap.parse_args(argv)

    module = serial.Serial(args.module, args.baud, timeout=0.02)
    module.setDTR(False)
    module.setRTS(False)
    decoder = proto.Decoder()

    print(f"=== S01 bench, {args.distance} ===")
    print(f"module {args.module}, {args.shots} shots every {args.rate} ms")

    print("resetting the module and opening the session")
    reset_board(module)
    time.sleep(1.5)

    info = wait_for_hello_ack(module, decoder)
    if info is None:
        print("the module never answered hello. Is the port right?")
        return 1
    print(f"module fw {'.'.join(str(b) for b in info['fw'])} mac {proto.mac_str(info['mac'])}")

    module.write(
        proto.encode("beacons", mask=args.beacons, mode=args.mode,
                     period_ms=args.period, slot=args.slot)
    )
    print(f"beacons mask 0x{args.beacons:02X} mode {args.mode} slot {args.slot}")

    # The run is started over the radio, so the pistol can be anywhere.
    module.write(proto.encode("bench_start", shots=args.shots, rate_ms=args.rate))
    print("start sent over the radio, waiting for the pistol to finish")

    # Generous: the pistol resends a lost shot up to three times at 20 ms,
    # so a bad run is slower than a good one, and the report itself takes a
    # further 100 ms or so.
    budget = args.shots * (args.rate + 4 * 25) / 1000.0 + 20.0

    frames = 0
    bad = 0
    pistol_seqs = set()
    report = None
    status = None
    deadline = time.monotonic() + budget
    started = time.monotonic()

    while time.monotonic() < deadline:
        for item in decoder.feed(module.read(4096)):
            if item[0] == "frame":
                _, type_, fields = item
                if type_ == proto.BY_NAME["shot"].type:
                    frames += 1
                    pistol_seqs.add(fields["pistol_seq"])
                    module.write(proto.encode("ack", seq=fields["seq"]))
                elif type_ == proto.BY_NAME["bench_report"].type:
                    report = fields
                elif type_ == proto.BY_NAME["status"].type:
                    status = fields
                elif type_ == proto.BY_NAME["error"].type:
                    print(f"  module error: {proto.describe(type_, fields)}")
            elif item[0] == "text" and args.verbose:
                print(f"  . {item[1]}")
            elif item[0] == "bad":
                bad += 1
        if report is not None:
            break

    elapsed = time.monotonic() - started

    if report is None:
        print(f"\nno report from the pistol inside {budget:.0f} s")
        print(f"the module forwarded {frames} shot frame(s) in that time")
        print("is the pistol powered, on channel 1, and within range?")
        return 1

    # Ask the module what it counted, now that the run is over.
    module.write(proto.encode("status_req"))
    until = time.monotonic() + 2.0
    while time.monotonic() < until and status is None:
        for item in decoder.feed(module.read(4096)):
            if item[0] == "frame" and item[1] == proto.BY_NAME["status"].type:
                status = item[2]

    sent = report["sent"]
    lost = report["lost"]
    loss_pct = (lost * 100.0 / sent) if sent else 0.0
    distinct = len(pistol_seqs)

    print(f"\nrun took {elapsed:.1f} s, report for run {report['run_id']}")

    print("\n--- section 1, shot to acknowledgement round trip ---")
    print(f"| {args.distance} | {sent} | {report['median_us']} | {report['p95_us']} | "
          f"{loss_pct:.2f} percent | {report['resends']} | "
          f"{report['rssi_at_pistol']} / {report['rssi_at_module']} |")
    print(f"  mean {report['mean_us']} us, min {report['min_us']} us, "
          f"max {report['max_us']} us, acked {report['acked']}, lost {lost}")

    print("\n--- section 2, forwarded frames against heard shots ---")
    heard = status.get("shots_heard", "?") if status else "?"
    core_acked = status.get("shots_acked", "?") if status else "?"
    unforwarded = "?" if heard == "?" else max(0, int(heard) - frames)
    print(f"| {args.distance} | {sent} | {heard} | {frames} | {core_acked} | {unforwarded} |")
    print(f"  distinct pistol sequence numbers in the forwarded frames: {distinct}")
    print(f"  frames the host could not decode: {bad}")

    ok = (frames == distinct == sent) and lost == 0
    print(f"\nzero loss and one frame per distinct shot: {'yes' if ok else 'NO'}")

    module.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
