#!/usr/bin/env python3
"""Record a node's serial output: contract messages (JSON lines) go to a file, everything else is shown.

    python tools/serial_capture.py --list                                # which serial ports exist
    python tools/serial_capture.py --port COM5 --out node.jsonl          # real board (Windows)
    python tools/serial_capture.py --port /dev/ttyUSB0 --out node.jsonl  # real board (Linux)
    python tools/serial_capture.py --port rfc2217://localhost:4000 --out node.jsonl   # Wokwi simulator

Then check the file:  python tools/contract_check.py --no-clock node.jsonl
(--no-clock because in M1 the node's clock is only an estimate.)

Close the PlatformIO serial monitor first: only one program can use a serial port at a time.
"""

import argparse
import sys
import time


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="serial port (COM5, /dev/ttyUSB0) or URL (rfc2217://localhost:4000)")
    p.add_argument("--out", default="node.jsonl", help="file for the JSON lines, default node.jsonl")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--seconds", type=float, default=None, help="stop after this many seconds")
    p.add_argument("--echo", action="store_true", help="also show the JSON lines on screen")
    p.add_argument("--list", action="store_true", help="list serial ports and exit")
    args = p.parse_args(argv)

    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("pyserial is missing: python -m pip install pyserial", file=sys.stderr)
        return 2

    if args.list or not args.port:
        ports = list(list_ports.comports())
        for port in ports:
            print(f"{port.device:15} {port.description}")
        if not ports:
            print("no serial ports found")
        return 0 if args.list else 2

    try:
        link = serial.serial_for_url(args.port, baudrate=args.baud, timeout=1)
    except (serial.SerialException, ValueError) as e:
        print(f"Cannot open {args.port}: {e}", file=sys.stderr)
        return 2

    count = 0
    end = None if args.seconds is None else time.monotonic() + args.seconds
    print(f"Recording {args.port} into {args.out}. Ctrl-C to stop.", file=sys.stderr)
    with link, open(args.out, "w", encoding="utf-8", newline="\n") as out:
        try:
            while end is None or time.monotonic() < end:
                raw = link.readline()
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace").strip()
                if text.startswith("{"):
                    out.write(text + "\n")
                    out.flush()
                    count += 1
                    if args.echo:
                        print(text)
                elif text:
                    print(text)
        except KeyboardInterrupt:
            pass
    print(f"{count} JSON line(s) saved to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())