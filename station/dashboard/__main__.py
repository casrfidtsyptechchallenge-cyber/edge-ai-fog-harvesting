"""Command line for M10. Run from the project folder:

    python -m station.dashboard                                  live, from the broker on this computer
    python -m station.dashboard --host 0.0.0.0                   on the Pi: phones on the hotspot can open it
    python -m station.dashboard --replay run.jsonl --speed 4     a recorded run, four times faster

Then open http://localhost:8080 (on another device: http://<the Pi's address>:8080). Add ?view=jury for the
jury screen. Set the environment variable AEROION_DASHBOARD_PASSWORD to ask for a password.
"""

import argparse
import os
import sys

from . import server, state


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--broker", default="localhost", help="MQTT broker, default localhost")
    p.add_argument("--port", type=int, default=1883, help="MQTT port, default 1883")
    p.add_argument("--replay", help="play a recorded run (JSON lines) instead of listening to the broker")
    p.add_argument("--speed", type=float, default=1.0, help="replay speed: 4 = four times faster, 0 = all at once")
    p.add_argument("--host", default="127.0.0.1",
                   help="address the page is served on; 0.0.0.0 = every network (the Pi's hotspot)")
    p.add_argument("--http-port", type=int, default=8080, help="port of the page, default 8080")
    p.add_argument("--history-min", type=float, default=10, help="minutes shown in the charts, default 10")
    p.add_argument("--read-only", action="store_true", help="hide the LOCK and TARE buttons")
    args = p.parse_args(argv)

    station = state.StationState(history_s=args.history_min * 60)
    hub = server.Hub(station)
    if args.replay:
        if not os.path.isfile(args.replay):
            print(f"{args.replay}: no such file", file=sys.stderr)
            return 2
        source = server.ReplaySource(hub, args.replay, args.speed)
    else:
        source = server.MqttSource(hub, args.broker, args.port)
    if args.read_only:
        hub.send_command = None

    password = os.environ.get("AEROION_DASHBOARD_PASSWORD") or None
    try:
        httpd = server.make_server(hub, args.host, args.http_port, password)
    except OSError as e:
        print(f"Cannot serve on {args.host}:{args.http_port}: {e} (another program on that port? try --http-port)",
              file=sys.stderr)
        return 2
    source.start()
    hub.run_ticker()

    shown = "localhost" if args.host in ("127.0.0.1", "0.0.0.0") else args.host
    print(f"Dashboard: http://{shown}:{args.http_port}   jury view: http://{shown}:{args.http_port}/?view=jury")
    print(f"Data: {'replay of ' + args.replay if args.replay else f'MQTT broker {args.broker}:{args.port}'}")
    if args.host == "0.0.0.0":
        print("Served on every network of this computer (the Pi's hotspot included).")
    if password:
        print("Password asked (AEROION_DASHBOARD_PASSWORD); any user name.")
    elif args.host != "127.0.0.1":
        print("No password: anyone on the network can see it" +
              (" and send LOCK or TARE." if hub.send_command else ".") +
              " Set AEROION_DASHBOARD_PASSWORD to ask for one.")
    print("Ctrl-C to stop.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        hub.close()
        httpd.server_close()
        if not args.replay:
            source.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())