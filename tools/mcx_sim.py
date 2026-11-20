#!/usr/bin/env python3
"""The MCX end of the node's UART, played by the PC (M7): to test the ESP32's link firmware in Wokwi.

    python tools/mcx_sim.py --port rfc2217://localhost:4000               a simulated MCX (node n01)
    python tools/mcx_sim.py --port rfc2217://localhost:4000 --relay COM5  your real MCX board instead

In firmware/node/wokwi/link, Wokwi's serial monitor is wired to the ESP32's GPIO 17/18 (the UART that goes to
the MCX on the real node) and shared on TCP port 4000. Start the simulation first, then this tool.

- Simulated MCX: one node of sim/fake_node.py sends telemetry, events, status and info as link lines, and
  obeys the commands the ESP32 passes on, answering with CMD_ACK. The ESP32's time lines are shown.
- Relay (--relay COM5): the real MCX board on its USB port talks to the simulated ESP32. Only JSON lines
  cross; the '#' lines of both chips are shown here, and what you type goes to the MCX's console.
- Radio (part 3): Wokwi has no radio between boards, so in the Wokwi build the ESP32 also writes its FOG_ALERT
  packets on this port ({"link":"radio","hex":...}); they are shown here and never reach the MCX. Type
  "alert n02 0.6" to play a neighbour: its FOG_ALERT goes to the ESP32, which pre-arms this node's MCX.
- Clocks: Wokwi often runs slower than real time, so the simulated ESP32's clock falls behind the PC's. A real
  node lives on the ESP32's clock (the MCX gets it from the time lines), so this tool does too: the neighbour's
  alerts carry the ESP32's time, and the simulated MCX judges a command's age by the ESP32's clock.

The ESP32's own log ('#' lines) also comes out on this port in the Wokwi build, so it is shown here.
"""

import argparse
import json
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "contract"))
sys.path.insert(0, str(ROOT / "sim"))
import aeroion_contract as ac  # noqa: E402


def compact(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


class LinkSink:
    """A sink for sim/fake_node.Simulation: each message becomes one link line to the ESP32."""

    def __init__(self, write_line):
        self.write_line = write_line
        self.commands = []  # (node_id, topic, payload) received from the ESP32, for Simulation.step()

    def publish(self, topic, payload, retain=False):
        self.write_line(compact({"topic": topic, "retain": retain, "payload": payload}))

    def poll_commands(self):
        commands, self.commands = self.commands, []
        return commands

    def close(self):
        pass


def read_esp_line(text, node_id):
    """What a line from the ESP32 is: ("time", ts), ("cmd", (topic, payload)), ("radio", alert), ("log", text)
    or ("bad", text). alert: a FOG_ALERT the ESP32 broadcast, as ac.unpack_fog_alert gives it.

    A command for another node is ("bad", ...): the ESP32 only passes on its own node's commands.
    """
    text = text.strip()
    if not text.startswith("{"):
        return "log", text
    try:
        line = json.loads(text)
    except ValueError:
        return "bad", text
    if not isinstance(line, dict):
        return "bad", text
    if line.get("link") == "time" and isinstance(line.get("ts"), int):
        return "time", line["ts"]
    if line.get("link") == "radio" and isinstance(line.get("hex"), str):
        try:
            return "radio", ac.unpack_fog_alert(bytes.fromhex(line["hex"]))
        except ValueError:
            return "bad", text
    topic, payload = line.get("topic"), line.get("payload")
    if topic in (ac.node_topic(node_id, "cmd"), ac.node_topic(ac.BROADCAST, "cmd")) and isinstance(payload, dict):
        return "cmd", (topic, payload)
    return "bad", text


def radio_line(node_id, fog_idx, seq, ts_ms, time_synced=True):
    """A neighbour's FOG_ALERT as the Wokwi build of the ESP32 reads it from the link."""
    packet = ac.pack_fog_alert(node_id, ts_ms, seq, fog_idx, time_synced=time_synced)
    return compact({"link": "radio", "hex": packet.hex().upper()})


def describe_alert(alert):
    when = time.strftime("%H:%M:%S", time.gmtime(alert["ts_ms"] / 1000))
    return (f"[radio] FOG_ALERT broadcast by {alert['node_id']}: fog {round(alert['fog_idx'] * 100)} %, "
            f"seq {alert['seq']}, seen {when} UTC{'' if alert['time_synced'] else ' (clock not synced)'}"
            f"{', virtual sensor' if alert['virtual_source'] else ''}")


class Neighbour:
    """The keyboard command "alert [nXX] [fog]": a neighbour's FOG_ALERT, as the radio would bring it."""

    USAGE = "alert [node] [fog]   a neighbour saw fog: e.g. alert n02 0.6 (this node gets a PREARM)"

    def __init__(self, own_node):
        self.own_node = own_node
        self.seq = 0

    def line_for(self, typed, esp_time_ms=None):
        """The radio line for a typed command, None if it is not an alert command; ValueError if it is wrong.

        esp_time_ms: the ESP32's clock (its latest time line). A neighbour synced by NTP agrees with it; with
        no time line yet, the neighbour has no time either.
        """
        words = typed.split()
        if not words or words[0] != "alert":
            return None
        node = words[1] if len(words) > 1 else ("n02" if self.own_node != "n02" else "n01")
        if not ac.is_valid_node_id(node):
            raise ValueError(f"invalid node id {node!r}: n01 to n99")
        if node == self.own_node:
            raise ValueError(f"{node} is this node: the ESP32 ignores its own alerts")
        try:
            fog = float(words[2]) if len(words) > 2 else 0.6
        except ValueError:
            raise ValueError(f"fog must be a number from 0 to 1, not {words[2]!r}") from None
        if not 0.0 <= fog <= 1.0:
            raise ValueError("fog must be from 0 to 1")
        self.seq = (self.seq + 1) & 0xFFFF
        if esp_time_ms is None:
            return radio_line(node, fog, self.seq, int(time.time() * 1000), time_synced=False)
        return radio_line(node, fog, self.seq, esp_time_ms)


class SimulatedMcx:
    """One simulated node behind the link: what the real MCX firmware does, from sim/fake_node.py."""

    def __init__(self, node_id, write_line, show=print, seed=None, compress=4.0):
        from fake_node import Simulation  # sim/fake_node.py

        self.node_id = node_id
        self.show = show
        self.sink = LinkSink(write_line)
        number = ac.node_number(node_id)
        positions = [(0.0, 0.0)] * number  # Simulation names its nodes n01, n02...: keep only ours
        self.sim = Simulation(self.sink, nodes=number, seed=seed, compress=compress, positions=positions)
        for other in [n for n in self.sim.nodes if n != node_id]:
            del self.sim.nodes[other]
        self.last_time = None   # the ESP32's clock, from its latest time line
        self.offset_ms = None   # the simulation's clock (the PC's) minus the ESP32's

    def start(self):
        self.sim.start()  # info and status, as the MCX sends them at boot

    def step(self):
        self.sim.step()  # one simulated second: telemetry, events, status every 10 s, commands answered

    def from_esp(self, text):
        kind, value = read_esp_line(text, self.node_id)
        if kind == "time":
            if self.last_time is None:
                self.show(f"[mcx] time from the ESP32: {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(value / 1000))}"
                          f" UTC (the ESP32 has NTP)")
            self.last_time = value
            self.offset_ms = round(self.sim.t * 1000) - value
        elif kind == "cmd":
            topic, payload = value
            self.show(f"[mcx] command {payload.get('type', '?')} {payload.get('cmd_id', '?')} from the ESP32")
            ts = payload.get("ts")
            if self.offset_ms is not None and isinstance(ts, int) and not isinstance(ts, bool):
                # the real MCX judges the age on the ESP32's clock: move ts to the simulation's clock
                payload = dict(payload, ts=ts + self.offset_ms)
            self.sink.commands.append((self.node_id, topic, payload))
        elif kind == "radio":
            self.show(describe_alert(value))
        elif kind == "log":
            if value:
                self.show(f"[esp] {value}")
        else:
            self.show(f"[mcx] not a link line, ignored: {value[:100]}")


def relay_line(text, source):
    """A line in relay mode: (forward it, what to show). source is "esp" or "mcx". Only JSON lines cross,
    except the ESP32's radio packets: they are for the neighbours, not the MCX."""
    text = text.strip()
    if not text:
        return False, None
    if text.startswith("{"):
        if source == "esp" and '"radio"' in text:
            kind, value = read_esp_line(text, "n01")
            if kind == "radio":
                return False, describe_alert(value)
        return True, None
    return False, f"[{source}] {text}"


class Lines:
    """Bytes from a serial port, cut into lines."""

    def __init__(self):
        self.pending = b""

    def feed(self, data):
        self.pending += data
        lines = []
        while b"\n" in self.pending:
            raw, self.pending = self.pending.split(b"\n", 1)
            lines.append(raw.decode("utf-8", errors="replace").rstrip("\r"))
        if len(self.pending) > 8192:
            self.pending = b""  # no line end for too long: noise
        return lines


_show_lock = threading.Lock()


def show(text):
    with _show_lock:
        sys.stdout.write(text + "\n")
        sys.stdout.flush()


def open_port(serial, url, baud):
    try:
        return serial.serial_for_url(url, baudrate=baud, timeout=0.05)
    except (serial.SerialException, ValueError) as e:
        print(f"Cannot open {url}: {e}", file=sys.stderr)
        if url.startswith("rfc2217://"):
            print("Is the Wokwi simulation running, with rfc2217ServerPort = 4000 in wokwi.toml?", file=sys.stderr)
        return None


def run_simulated(esp, args):
    lock = threading.Lock()

    def to_esp(line):
        with lock:
            esp.write((line + "\n").encode("utf-8"))

    mcx = SimulatedMcx(args.node, to_esp, show, seed=args.seed, compress=args.compress)
    neighbour = Neighbour(args.node)

    def keyboard():
        for typed in sys.stdin:
            try:
                line = neighbour.line_for(typed.strip(), mcx.last_time)
            except ValueError as e:
                show(f"[radio] {e}")
                continue
            if line is not None:
                show(f"[radio] {typed.strip()}: FOG_ALERT to the ESP32")
                to_esp(line)
            elif typed.strip():
                show(f"[mcx] commands: {Neighbour.USAGE}")

    threading.Thread(target=keyboard, daemon=True).start()
    show(f"[mcx] simulated MCX of node {args.node} on {args.port}. Type: {Neighbour.USAGE}. Ctrl-C to stop.")
    mcx.start()
    lines = Lines()
    next_step = time.monotonic() + 1.0
    while True:
        for line in lines.feed(esp.read(esp.in_waiting or 1)):
            mcx.from_esp(line)
        if time.monotonic() >= next_step:
            next_step += 1.0
            mcx.step()


def run_relay(esp, mcx, args):
    lock = threading.Lock()  # one writer at a time on each port
    esp_lock = threading.Lock()
    neighbour = Neighbour(args.node)
    esp_clock = {"ms": None}  # the ESP32's latest time line

    def keyboard():
        for typed in sys.stdin:
            typed = typed.strip()
            try:
                line = neighbour.line_for(typed, esp_clock["ms"])
            except ValueError as e:
                show(f"[radio] {e}")
                continue
            if line is not None:
                show(f"[radio] {typed}: FOG_ALERT to the ESP32")
                with esp_lock:
                    esp.write((line + "\n").encode("utf-8"))
            elif typed:
                with lock:
                    mcx.write((typed + "\n").encode("utf-8"))

    threading.Thread(target=keyboard, daemon=True).start()
    show(f"[relay] ESP32 {args.port} <-> MCX {args.relay}. Type MCX console commands (help), or "
         f"{Neighbour.USAGE}. Ctrl-C to stop.")
    from_esp, from_mcx = Lines(), Lines()
    while True:
        for line in from_esp.feed(esp.read(esp.in_waiting or 1)):
            if line.startswith('{"link":"time"'):
                kind, value = read_esp_line(line, args.node)
                if kind == "time":
                    esp_clock["ms"] = value
            forward, text = relay_line(line, "esp")
            if forward:
                with lock:
                    mcx.write((line + "\n").encode("utf-8"))
            if text:
                show(text)
        for line in from_mcx.feed(mcx.read(mcx.in_waiting or 1)):
            forward, text = relay_line(line, "mcx")
            if forward:
                with esp_lock:
                    esp.write((line + "\n").encode("utf-8"))
            if text:
                show(text)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="rfc2217://localhost:4000", help="the ESP32's link UART (Wokwi: the default)")
    p.add_argument("--relay", help="serial port of a real MCX board (COM5): relay instead of simulating")
    p.add_argument("--node", default="n01", help="node id, the same as LINK_NODE_ID in link_config.h")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--compress", type=float, default=4.0, help="simulated fog comes this much faster, default 4")
    p.add_argument("--seed", type=int, default=None)
    args = p.parse_args(argv)
    if not ac.is_valid_node_id(args.node):
        print(f"invalid node id {args.node!r}: n01 to n99", file=sys.stderr)
        return 2
    try:
        import serial
    except ImportError:
        print("pyserial is missing: python -m pip install pyserial", file=sys.stderr)
        return 2

    esp = open_port(serial, args.port, args.baud)
    if esp is None:
        return 2
    mcx = None
    if args.relay:
        mcx = open_port(serial, args.relay, args.baud)
        if mcx is None:
            esp.close()
            return 2
    try:
        if mcx is None:
            run_simulated(esp, args)
        else:
            run_relay(esp, mcx, args)
    except KeyboardInterrupt:
        pass
    except (serial.SerialException, OSError) as e:
        print(f"\nThe serial connection stopped: {e} (did the simulation stop?)", file=sys.stderr)
        return 1
    finally:
        esp.close()
        if mcx is not None:
            mcx.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())