"""Command line for the badge list (M4, station side). Run from the project folder:

    python -m station.badges add 04A1B2C3 operator --label "white card"   add a badge, or change its role
    python -m station.badges remove 04A1B2C3
    python -m station.badges list
    python -m station.badges check                                         check the file
    python -m station.badges sync --node all                               send the list to every node
    python -m station.badges sync --node n01 --print                       only print the command line

Every change raises the list's version. Run sync after changes: nodes keep the last list they received.
"""

import argparse
import json
import sys
import threading
import time

from . import registry as reg  # noqa: F401  (also puts contract/ on the import path)

import aeroion_contract as ac  # noqa: E402


def cmd_add(args):
    registry = reg.load(args.file)
    if not reg.add(registry, args.uid, args.role, args.label):
        print(f"No change: {reg.normalize_uid(args.uid)} is already {args.role}.")
        return 0
    reg.save(registry, args.file)
    print(f"{reg.normalize_uid(args.uid)} is now {args.role}. List version {registry['version']}: "
          f"run 'python -m station.badges sync --node all' to update the nodes.")
    return 0


def cmd_remove(args):
    registry = reg.load(args.file)
    if not reg.remove(registry, args.uid):
        print(f"{reg.normalize_uid(args.uid)} is not on the list.")
        return 1
    reg.save(registry, args.file)
    print(f"Removed. List version {registry['version']}: run sync to update the nodes.")
    return 0


def cmd_list(args):
    registry = reg.load(args.file)
    print(f"{args.file}: version {registry['version']}, {len(registry['badges'])} badge(s)")
    for b in registry["badges"]:
        print(f"  {b['uid']:20}  {b['role']:10}  {b.get('label', '')}")
    return 0


def cmd_check(args):
    registry = reg.load(args.file)  # raises BadgeError with every problem
    print(f"OK: version {registry['version']}, {len(registry['badges'])} badge(s), "
          f"at most {reg.MAX_BADGES} allowed")
    return 0


def send_and_wait(cmd, topic, broker, port, wait_s):
    """Publish one command on MQTT and collect the CMD_ACK answers. None if the broker cannot be reached."""
    import paho.mqtt.client as mqtt

    acks, subscribed = [], threading.Event()
    if hasattr(mqtt, "CallbackAPIVersion"):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    else:
        client = mqtt.Client()

    def on_message(_client, _userdata, msg):
        try:
            event = json.loads(msg.payload)
        except ValueError:
            return
        if event.get("type") == "CMD_ACK" and event.get("data", {}).get("cmd_id") == cmd["cmd_id"]:
            acks.append(event)

    client.on_connect = lambda c, *a: c.subscribe(f"{ac.TOPIC_ROOT}/+/event", qos=1)
    client.on_subscribe = lambda *a: subscribed.set()
    client.on_message = on_message
    try:
        client.connect(broker, port, 30)
    except OSError:
        return None
    client.loop_start()
    subscribed.wait(timeout=3)
    client.publish(topic, json.dumps(cmd), qos=1).wait_for_publish(timeout=3)
    time.sleep(wait_s)
    client.loop_stop()
    client.disconnect()
    return acks


def cmd_sync(args):
    registry = reg.load(args.file)
    cmd = reg.sync_command(registry, ttl_s=args.ttl)
    topic = ac.node_topic(args.node, "cmd")
    if args.print:
        # One contract line, the format of tools/serial_capture.py and of the ESP32-MCX link (M7).
        print(json.dumps({"topic": topic, "retain": False, "payload": cmd}, separators=(",", ":")))
        return 0
    acks = send_and_wait(cmd, topic, args.broker, args.port, args.wait)
    if acks is None:
        print(f"Cannot reach the MQTT broker at {args.broker}:{args.port}.", file=sys.stderr)
        return 2
    print(f"sent badge list version {registry['version']} ({len(registry['badges'])} badge(s)) to {topic}")
    if not acks:
        print("no answer: is the node (or the simulator) running?")
        return 1
    refused = 0
    for a in sorted(acks, key=lambda e: e["node_id"]):
        d = a["data"]
        print(f"  {a['node_id']}: {d['result']}" + (f" ({d['reason']})" if d["reason"] else ""))
        refused += d["result"] != "accepted"
    return 1 if refused else 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="python -m station.badges", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--file", default=str(reg.DEFAULT_FILE), help="badge list, default data/badges.json")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("add", help="add a badge, or change its role or label")
    s.add_argument("uid", help="as the node prints it, e.g. 04A1B2C3 (spaces, colons and lower case are fine)")
    s.add_argument("role", choices=ac.ROLES)
    s.add_argument("--label", default="", help="which card it is, e.g. 'white card' (no personal names)")
    s.set_defaults(run=cmd_add)

    s = sub.add_parser("remove", help="remove a badge")
    s.add_argument("uid")
    s.set_defaults(run=cmd_remove)

    sub.add_parser("list", help="show the list").set_defaults(run=cmd_list)
    sub.add_parser("check", help="check the file").set_defaults(run=cmd_check)

    s = sub.add_parser("sync", help="send the list to the nodes (SYNC_ALLOWLIST)")
    s.add_argument("--node", required=True, help="node id such as n01, or 'all'")
    s.add_argument("--broker", default="localhost")
    s.add_argument("--port", type=int, default=1883)
    s.add_argument("--ttl", type=int, default=reg.DEFAULT_TTL_S, help="seconds before the command expires")
    s.add_argument("--wait", type=float, default=3.0, help="seconds to wait for the answers, default 3")
    s.add_argument("--print", action="store_true", help="only print the command as one JSON line")
    s.set_defaults(run=cmd_sync)

    args = p.parse_args(argv)
    try:
        return args.run(args)
    except reg.BadgeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())