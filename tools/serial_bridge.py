#!/usr/bin/env python3
"""Serial-to-MQTT bridge (M7): the PC does the job of the node's ESP32 for an MCX board on USB.

    python tools/serial_bridge.py --list                              which serial ports exist
    python tools/serial_bridge.py --port COM5                         node n01, MQTT broker on this PC
    python tools/serial_bridge.py --port COM5 --node n02 --broker 192.168.1.20

What it does, as the node's ESP32 will (docs/link.md):
- each JSON line from the board (one contract message) is published on MQTT, with its retain flag;
- commands for the node (aeroion/<node>/cmd and aeroion/all/cmd) go to the board as one JSON line each;
- every 5 s it sends the time, {"link":"time","ts":<Unix ms>}: the board's clock is synced and its link is up;
- lines starting with '#' are shown here, and what you type here goes to the board (its console commands);
- if the bridge stops, the broker publishes the node's last will: status with online false.

Close any other serial terminal first: only one program can use a serial port at a time.
"""

import argparse
import json
import signal
import sys
import threading
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "contract"))
import aeroion_contract as ac  # noqa: E402

LINE_MAX = 4095  # the longest line the MCX takes (LINK_LINE_MAX in mcx_node_config.h)
TIME_EVERY_S = 5.0  # the MCX reports COMM_LOST after 15 s without a time line


def compact(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


class Bridge:
    """Everything the bridge decides, without the serial port and the MQTT client, so tests can drive it.

    publish(topic, text, qos, retain) sends on MQTT, to_board(text) writes one line to the board, and
    show(text) tells the person at the PC.
    """

    def __init__(self, node_id, publish, to_board, show=print, check=True):
        if not ac.is_valid_node_id(node_id):
            raise ValueError(f"invalid node id {node_id!r}: n01 to n99")
        self.node_id = node_id
        self.publish = publish
        self.to_board = to_board
        self.show = show
        self.check = check
        self.counts = Counter()
        self._warned_other_node = False

    def subscriptions(self):
        """The command topics this node listens to."""
        return [ac.node_topic(self.node_id, "cmd"), ac.node_topic(ac.BROADCAST, "cmd")]

    def last_will(self):
        """(topic, text) the broker publishes, retained, if the bridge disappears."""
        payload = {"schema_version": ac.SCHEMA_VERSION, "node_id": self.node_id, "online": False}
        return ac.node_topic(self.node_id, "status"), compact(payload)

    @staticmethod
    def time_line(now=None):
        """The time for the board, in Unix milliseconds."""
        return compact({"link": "time", "ts": int((time.time() if now is None else now) * 1000)})

    def from_board(self, raw):
        """One line from the board: a contract message is published, anything else is shown."""
        text = raw.decode("utf-8", errors="replace") if isinstance(raw, bytes) else raw
        text = text.strip()
        if not text:
            return
        if not text.startswith("{"):
            self.show(text)  # '#' lines: for people
            return
        try:
            line = json.loads(text)
            topic, payload = line["topic"], line["payload"]
            retain = bool(line.get("retain", False))
            scope, kind = ac.parse_topic(topic)
            if kind not in ac.NODE_KINDS or kind == "cmd" or not isinstance(payload, dict):
                raise ValueError(f"a node does not send {topic}")
        except (ValueError, KeyError, TypeError) as e:
            self.counts["not published"] += 1
            self.show(f"! not a contract line from the board ({e}): {text[:100]}")
            return
        if scope != self.node_id and not self._warned_other_node:
            self._warned_other_node = True
            self.show(f"! the board is {scope}, but this bridge sends it the commands of {self.node_id}: "
                      f"start it again with --node {scope}")
        if self.check:
            problems = ac.validate(kind, payload)
            if problems:
                self.counts["invalid"] += 1
                self.show(f"! {topic} breaks the contract: {problems[0]}")
        qos = 0 if kind in ("telemetry", "wave") else 1
        self.publish(topic, compact(payload), qos, retain)
        self.counts[kind] += 1

    def from_broker(self, topic, raw):
        """A command from MQTT: to the board as one line. The board checks it and answers with CMD_ACK."""
        try:
            cmd = json.loads(raw)
            if not isinstance(cmd, dict):
                raise ValueError("not a JSON object")
        except ValueError as e:
            self.show(f"! {topic}: not a command ({e}), not sent to the board")
            return
        if self.check:
            problems = ac.validate("cmd", cmd)
            if problems:  # still sent: the node answers "rejected" with the reason, as the contract wants
                self.show(f"! {topic} breaks the contract: {problems[0]}")
        line = compact({"topic": topic, "retain": False, "payload": cmd})
        if len(line.encode("utf-8")) > LINE_MAX:
            self.show(f"! {cmd.get('type', '?')} {cmd.get('cmd_id', '?')}: {len(line)} characters, more than the "
                      f"board takes ({LINE_MAX}); not sent")
            return
        self.to_board(line)
        self.counts["commands"] += 1
        self.show(f"> {cmd.get('type', '?')} {cmd.get('cmd_id', '?')} sent to the board")

    def typed(self, text):
        """A line typed at the PC: a console command for the board."""
        text = text.strip()
        if text:
            self.to_board(text)


def make_client(mqtt, client_id):
    if hasattr(mqtt, "CallbackAPIVersion"):  # paho-mqtt 2.x
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    return mqtt.Client(client_id=client_id)


_show_lock = threading.Lock()


def show(text):
    with _show_lock:  # the serial loop and the MQTT thread both print: one whole line at a time
        sys.stdout.write(text + "\n")
        sys.stdout.flush()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="serial port of the board (COM5, /dev/ttyACM0)")
    p.add_argument("--node", default="n01", help="the board's node id, default n01 (NODE_ID in mcx_node_config.h)")
    p.add_argument("--broker", default="localhost", help="MQTT broker, default localhost")
    p.add_argument("--mqtt-port", type=int, default=1883)
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--no-check", action="store_true", help="do not check the messages against the contract")
    p.add_argument("--list", action="store_true", help="list serial ports and exit")
    args = p.parse_args(argv)

    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("pyserial is missing: python -m pip install pyserial", file=sys.stderr)
        return 2
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("paho-mqtt is missing: python -m pip install paho-mqtt", file=sys.stderr)
        return 2

    if args.list or not args.port:
        ports = list(list_ports.comports())
        for port in ports:
            print(f"{port.device:15} {port.description}")
        if not ports:
            print("no serial ports found")
        if not args.list:
            print("Give the board's port with --port, e.g. --port COM5 (the MCU-Link port).", file=sys.stderr)
        return 0 if args.list else 2

    try:
        board = serial.serial_for_url(args.port, baudrate=args.baud, timeout=0.2)
    except (serial.SerialException, ValueError) as e:
        print(f"Cannot open {args.port}: {e}", file=sys.stderr)
        return 2

    write_lock = threading.Lock()
    state = {"board": board}

    def to_board(text):
        with write_lock:
            try:
                state["board"].write((text + "\n").encode("utf-8"))
            except (serial.SerialException, OSError):
                pass  # the main loop notices and reopens the port

    client = make_client(mqtt, f"aeroion-bridge-{args.node}")
    bridge = Bridge(args.node, lambda t, text, q, r: client.publish(t, text, qos=q, retain=r), to_board, show,
                    check=not args.no_check)
    will_topic, will_text = bridge.last_will()
    client.will_set(will_topic, will_text, qos=1, retain=True)

    def on_connect(c, *_):
        for topic in bridge.subscriptions():
            c.subscribe(topic, qos=1)
        show(f"# bridge: MQTT connected to {args.broker}:{args.mqtt_port}, listening to "
             f"{' and '.join(bridge.subscriptions())}")

    client.on_connect = on_connect
    client.on_disconnect = lambda *_: show("# bridge: MQTT connection lost, trying again")
    client.on_message = lambda _c, _u, msg: bridge.from_broker(msg.topic, msg.payload)
    try:
        client.connect(args.broker, args.mqtt_port, keepalive=30)
    except OSError as e:
        print(f"Cannot reach the MQTT broker at {args.broker}:{args.mqtt_port} ({e}). Is mosquitto running?",
              file=sys.stderr)
        board.close()
        return 2
    client.loop_start()

    def keyboard():
        for typed in sys.stdin:
            bridge.typed(typed)

    threading.Thread(target=keyboard, daemon=True).start()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))  # stopped by a service manager: clean exit too
    show(f"# bridge: {args.port} <-> MQTT as node {args.node}. Type board commands (help); Ctrl-C to stop.")

    pending = b""
    next_time = 0.0
    try:
        while True:
            if time.monotonic() >= next_time:
                next_time = time.monotonic() + TIME_EVERY_S
                to_board(bridge.time_line())
            try:
                pending += state["board"].read(state["board"].in_waiting or 1)
            except (serial.SerialException, OSError) as e:
                show(f"# bridge: lost {args.port} ({e}); trying again every 2 s")
                client.publish(will_topic, will_text, qos=1, retain=True)
                with write_lock:
                    state["board"].close()
                while True:
                    time.sleep(2)
                    try:
                        with write_lock:
                            state["board"] = serial.serial_for_url(args.port, baudrate=args.baud, timeout=0.2)
                        break
                    except (serial.SerialException, ValueError):
                        continue
                show(f"# bridge: {args.port} is back")
                pending, next_time = b"", 0.0
                continue
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                bridge.from_board(raw)
            if len(pending) > 2 * LINE_MAX:
                pending = b""  # no line end for too long: noise
    except KeyboardInterrupt:
        pass
    finally:
        try:  # the node is offline now: say so at once, as the last will would
            client.publish(will_topic, will_text, qos=1, retain=True).wait_for_publish(timeout=2)
        except (RuntimeError, ValueError):
            pass  # the broker is gone too
        client.loop_stop()
        client.disconnect()
        state["board"].close()
        summary = ", ".join(f"{n} {k}" for k, n in sorted(bridge.counts.items())) or "nothing"
        print(f"\n# bridge stopped: {summary}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())