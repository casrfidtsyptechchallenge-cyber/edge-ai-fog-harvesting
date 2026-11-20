#!/usr/bin/env python3
"""Send one command to a node (or to all nodes) and print the node's CMD_ACK answer.

    python tools/send_cmd.py --node n02 lock "testing the stop button"
    python tools/send_cmd.py --node all lock "emergency stop"
    python tools/send_cmd.py --node n03 prearm --eta 20 --source n01
    python tools/send_cmd.py --node n01 hint 0.6
    python tools/send_cmd.py --node n01 tare

The command is built and validated against the contract before it is sent, so this is also a reference
for how the station services should build commands.
"""

import argparse
import json
import sys
import threading
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "contract"))
import aeroion_contract as ac  # noqa: E402


def build_command(ctype, params, ttl_s=30):
    return {"schema_version": ac.SCHEMA_VERSION, "cmd_id": uuid.uuid4().hex[:12], "ts": int(time.time() * 1000),
            "ttl_s": ttl_s, "type": ctype, "params": params}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--node", required=True, help="node id such as n01, or 'all'")
    p.add_argument("--broker", default="localhost")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--ttl", type=int, default=30, help="seconds before the command expires, default 30")
    p.add_argument("--wait", type=float, default=3.0, help="seconds to wait for acknowledgements, default 3")
    sub = p.add_subparsers(dest="type", required=True)
    s = sub.add_parser("lock", help="go to LOCKED now")
    s.add_argument("reason")
    s = sub.add_parser("prearm", help="fog is coming: lower the detection threshold")
    s.add_argument("--eta", type=float, required=True, help="expected arrival, seconds from now")
    s.add_argument("--source", required=True, help="node that saw the fog first")
    s = sub.add_parser("hint", help="suggest an HV level between 0 and 1")
    s.add_argument("level", type=float)
    sub.add_parser("tare", help="zero the load cell")
    args = p.parse_args(argv)

    params = {"lock": lambda: {"reason": args.reason},
              "prearm": lambda: {"eta_s": args.eta, "source_node": args.source},
              "hint": lambda: {"hv_level": args.level},
              "tare": lambda: {}}[args.type]()
    ctype = {"lock": "LOCK", "prearm": "PREARM", "hint": "SET_HINT", "tare": "TARE"}[args.type]
    cmd = build_command(ctype, params, args.ttl)
    errors = ac.validate("cmd", cmd)
    if errors:
        print("Not sent, the command breaks the contract:\n  " + "\n  ".join(errors), file=sys.stderr)
        return 1
    topic = ac.node_topic(args.node, "cmd")

    import paho.mqtt.client as mqtt

    acks, subscribed = [], threading.Event()
    if hasattr(mqtt, "CallbackAPIVersion"):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    else:
        client = mqtt.Client()

    def on_message(_c, _u, msg):
        event = json.loads(msg.payload)
        if event.get("type") == "CMD_ACK" and event["data"]["cmd_id"] == cmd["cmd_id"]:
            acks.append(event)

    client.on_connect = lambda c, *a: c.subscribe(f"{ac.TOPIC_ROOT}/+/event", qos=1)
    client.on_subscribe = lambda *a: subscribed.set()
    client.on_message = on_message
    try:
        client.connect(args.broker, args.port, 30)
    except OSError as e:
        print(f"Cannot reach the MQTT broker at {args.broker}:{args.port} ({e}).", file=sys.stderr)
        return 2
    client.loop_start()
    subscribed.wait(timeout=3)
    client.publish(topic, json.dumps(cmd), qos=1).wait_for_publish(timeout=3)
    print(f"sent {ctype} {cmd['cmd_id']} to {topic}")
    time.sleep(args.wait)
    client.loop_stop()
    client.disconnect()

    if not acks:
        print("no answer: is the node (or the simulator) running?")
        return 1
    for a in acks:
        d = a["data"]
        print(f"  {a['node_id']}: {d['result']}" + (f" ({d['reason']})" if d["reason"] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
