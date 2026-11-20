"""Command line for M8. Run from the project folder:

    python -m station.network                          live: track fog fronts, pre-arm the nodes ahead of them
    python -m station.network --prior 90 1.0           the site's usual fog moves toward 90° (east) at 1 m/s
    python -m station.network --no-prearm              track and publish only
    python -m station.network score run.jsonl          score the tracker on a simulator run made with --truth

The site's usual front is learned from the fronts measured on 3+ nodes and kept in data/network_prior.json.
"""

import argparse
import sys
import time

from . import score, service
from .engine import NetworkEngine
from .tracker import FrontTracker, Prior, TrackerSettings


def settings_from(args, keep_finished=50):
    return TrackerSettings(slow_speed_mps=args.slow_speed, prearm=not getattr(args, "no_prearm", False),
                           keep_finished=keep_finished)


def add_common(p):
    p.add_argument("--prior", nargs=2, type=float, metavar=("DEG", "MPS"),
                   help="the site's usual front: moving toward DEG (0 = north, 90 = east) at MPS m/s")
    p.add_argument("--slow-speed", type=float, default=0.5,
                   help="with no direction yet, pre-arm as if the fog came this slowly (m/s), default 0.5")


def main_score(argv):
    p = argparse.ArgumentParser(prog="python -m station.network score",
                                description="Score the tracker on a simulator run recorded with --truth.")
    p.add_argument("file", help="JSON lines from sim/fake_node.py --out ... --truth")
    add_common(p)
    args = p.parse_args(argv)
    prior = Prior.toward(args.prior[0], args.prior[1]) if args.prior else None
    report = score.score_run(args.file, FrontTracker(settings_from(args, keep_finished=10**6), prior))
    if not report["fronts_true"]:
        print("No simulator truth in this file: record it with sim/fake_node.py ... --truth", file=sys.stderr)
        return 2
    print(score.format_report(report))
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["score"]:
        return main_score(argv[1:])
    p = argparse.ArgumentParser(prog="python -m station.network", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--broker", default="localhost")
    p.add_argument("--port", type=int, default=1883)
    add_common(p)
    p.add_argument("--no-prearm", action="store_true", help="track and publish the front, send no PREARM")
    p.add_argument("--prior-file", default=str(service.DEFAULT_PRIOR_FILE),
                   help="where the learned usual front is kept, default data/network_prior.json")
    p.add_argument("--forget", action="store_true", help="start without the learned usual front")
    args = p.parse_args(argv)

    if args.prior:
        prior = Prior.toward(args.prior[0], args.prior[1])
        origin = f"set: toward {args.prior[0]:.0f}° at {args.prior[1]:.2f} m/s"
    else:
        prior = None if args.forget else service.load_prior(args.prior_file)
        origin = ("none yet: learned from the fronts measured on 3+ nodes" if prior is None else
                  f"learned from {prior.fronts} front(s): toward {prior.direction_deg:.0f}° at "
                  f"{prior.speed_mps:.2f} m/s, agreement {prior.agreement:.2f}")
    engine = NetworkEngine(FrontTracker(settings_from(args), prior))

    def log(text):
        print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)

    svc = service.NetworkService(engine, args.broker, args.port, args.prior_file, log)
    try:
        svc.start()
    except OSError as e:
        print(f"Cannot reach the MQTT broker at {args.broker}:{args.port} ({e}). Is Mosquitto running?",
              file=sys.stderr)
        return 2
    log(f"fog-front tracker on {args.broker}:{args.port}; pre-arming {'OFF' if args.no_prearm else 'on'}")
    log(f"site's usual front: {origin}")
    log("Ctrl-C to stop.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        svc.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())