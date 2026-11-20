#!/usr/bin/env python3
"""Check that messages follow the AeroIon-Edge contract.

Use it whenever a new piece of firmware or a station service starts publishing:

    # Live: watch everything on the broker
    python tools/contract_check.py --broker localhost

    # Offline: a file of JSON lines {"topic": ..., "payload": ...}, e.g. from the simulator or a serial log
    python sim/fake_node.py --out run.jsonl --no-wait --duration 600
    python tools/contract_check.py --no-clock run.jsonl

    # Or from a pipe (Linux/macOS): ... | python tools/contract_check.py --stdin

Besides schema errors it reports per-node sequence gaps (lost messages), repeated sequence numbers
(duplicates) and timestamps far from the station clock (unsynchronised node). When reading a file or
standard input, the exit code is 1 if any message broke the contract.
"""

import argparse
import json
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "contract"))
import aeroion_contract as ac  # noqa: E402

CLOCK_TOLERANCE_MS = 5_000


class Checker:
    def __init__(self, quiet=False, check_clock=True, out=sys.stdout):
        self.quiet = quiet
        self.check_clock = check_clock
        self.out = out
        self.counts = Counter()
        self.invalid = Counter()
        self.warnings = Counter()
        self.last_seq = {}

    def _report(self, level, topic, text):
        if level == "ERROR" or not self.quiet:
            print(f"{level:5} {topic}: {text}", file=self.out)

    def check(self, topic, payload):
        """Check one message; payload may be bytes, str or an already-decoded dict."""
        try:
            scope, kind = ac.parse_topic(topic)
        except ValueError as e:
            self.invalid["topic"] += 1
            self._report("ERROR", topic, str(e))
            return False
        if scope == ac.SIM:
            self.counts["sim"] += 1
            return True
        if not isinstance(payload, dict):
            try:
                payload = json.loads(payload)
            except (ValueError, UnicodeDecodeError) as e:
                self.invalid[kind] += 1
                self._report("ERROR", topic, f"not JSON: {e}")
                return False
        self.counts[kind] += 1

        errors = ac.validate(kind, payload)
        if scope not in (ac.STATION, ac.BROADCAST) and payload.get("node_id") not in (None, scope):
            errors.append(f"node_id {payload.get('node_id')!r} does not match the topic")
        if errors:
            self.invalid[kind] += 1
            for e in errors:
                self._report("ERROR", topic, e)
            return False

        if "seq" in payload and scope not in (ac.STATION, ac.BROADCAST):
            prev = self.last_seq.get(scope)
            seq = payload["seq"]
            if prev is not None and not payload.get("buffered"):
                if seq == prev:
                    self.warnings["duplicate"] += 1
                    self._report("WARN", topic, f"seq {seq} repeated (duplicate message)")
                elif seq > prev + 1:
                    self.warnings["gap"] += 1
                    self._report("WARN", topic, f"seq jumped {prev} -> {seq}: {seq - prev - 1} message(s) lost")
                elif seq < prev:
                    self.warnings["restart"] += 1
                    self._report("WARN", topic, f"seq went back {prev} -> {seq} (node restarted?)")
            self.last_seq[scope] = max(seq, prev or 0) if payload.get("buffered") else seq

        if self.check_clock and "ts" in payload and not payload.get("buffered") and kind != "cmd":
            skew = payload["ts"] - time.time() * 1000
            if abs(skew) > CLOCK_TOLERANCE_MS:
                self.warnings["clock"] += 1
                self._report("WARN", topic, f"ts is {skew / 1000:+.1f} s from the station clock")
        return True

    def summary(self):
        total = sum(self.counts.values())
        bad = sum(self.invalid.values())
        lines = [f"{total} message(s) checked, {bad} invalid"]
        for kind, n in sorted(self.counts.items()):
            lines.append(f"  {kind:10} {n:7}  invalid: {self.invalid.get(kind, 0)}")
        if self.invalid.get("topic"):
            lines.append(f"  bad topics       {self.invalid['topic']}")
        if self.warnings:
            lines.append("  warnings: " + ", ".join(f"{k} {v}" for k, v in sorted(self.warnings.items())))
        return "\n".join(lines)


def read_lines(path):
    """Read a text file whatever its encoding: UTF-8 (with or without BOM) or UTF-16, which is what
    Windows PowerShell's > redirection produces."""
    data = Path(path).read_bytes()
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = data.decode("utf-16")
    else:
        text = data.decode("utf-8-sig")
    return text.splitlines()


def run_lines(checker, lines):
    for lineno, line in enumerate(lines, 1):
        line = line.strip().lstrip("\ufeff")
        if not line:
            continue
        try:
            row = json.loads(line)
            topic, payload = row["topic"], row["payload"]
        except (ValueError, KeyError, TypeError):
            checker.invalid["line"] += 1
            print(f"ERROR line {lineno}: expected {{\"topic\": ..., \"payload\": ...}}", file=checker.out)
            continue
        checker.check(topic, payload)
    print(checker.summary(), file=checker.out)
    return 1 if sum(checker.invalid.values()) else 0


def run_mqtt(checker, host, port):
    import paho.mqtt.client as mqtt

    if hasattr(mqtt, "CallbackAPIVersion"):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="aeroion-contract-check")
    else:
        client = mqtt.Client(client_id="aeroion-contract-check")
    client.on_connect = lambda c, *a: c.subscribe(f"{ac.TOPIC_ROOT}/#", qos=1)
    client.on_message = lambda c, u, msg: checker.check(msg.topic, msg.payload)
    try:
        client.connect(host, port, 30)
    except OSError as e:
        print(f"Cannot reach the MQTT broker at {host}:{port} ({e}).", file=sys.stderr)
        return 2
    print(f"Checking aeroion/# on mqtt://{host}:{port}. Ctrl-C to stop.", file=sys.stderr)
    client.loop_start()
    try:
        while True:
            time.sleep(10)
            print(checker.summary(), file=checker.out)
    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        client.disconnect()
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("file", nargs="?", help="file of JSON lines to check (instead of listening to MQTT)")
    p.add_argument("--stdin", action="store_true", help="read JSON lines from standard input")
    p.add_argument("--broker", default="localhost", help="MQTT broker host, default localhost")
    p.add_argument("--port", type=int, default=1883, help="MQTT broker port, default 1883")
    p.add_argument("--quiet", action="store_true", help="print errors and the summary only, no warnings")
    p.add_argument("--no-clock", action="store_true",
                   help="skip the timestamp check (needed for simulated or replayed data)")
    args = p.parse_args(argv)
    checker = Checker(quiet=args.quiet, check_clock=not args.no_clock)
    if args.file:
        try:
            lines = read_lines(args.file)
        except OSError as e:
            print(f"Cannot read {args.file}: {e}", file=sys.stderr)
            return 2
        except UnicodeDecodeError:
            print(f"{args.file} is not a text file of JSON lines", file=sys.stderr)
            return 2
        return run_lines(checker, lines)
    if args.stdin:
        return run_lines(checker, sys.stdin)
    return run_mqtt(checker, args.broker, args.port)


if __name__ == "__main__":
    sys.exit(main())
