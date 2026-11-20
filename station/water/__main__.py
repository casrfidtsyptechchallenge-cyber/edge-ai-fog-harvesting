"""Command line for M9. Run from the project folder:

    python -m station.water --sim                     the water point without hardware (demo cards and filters)
    python -m station.water                           on the Pi: probes, valve, flow meter, RFID reader
    python -m station.water probe                     on the Pi: show what each part reads (bench check)

    python -m station.water quota "card 7" 25         this card may draw 25 L a day (a label or a UID)
    python -m station.water quota "card 7" --default  back to the default quota
    python -m station.water quotas                    the cards with their own quota
    python -m station.water filter add 04C3A1B2E35F80 --capacity 500 --label "filter 3"
    python -m station.water filter list
    python -m station.water report [--day 2026-11-20]  what was given out, to whom, and what was refused

Household cards are badges with the role household: python -m station.badges add 0A1B2C3D household --label "card 7"
"""

import argparse
import signal
import sys
import time

from station.badges import registry

from . import REPO
from .hardware import PiConfig, SimHardware
from .ledger import Ledger
from .point import BadgeBook, PointSettings, WaterPoint, card_name, local_day
from .quality import Limits, QualityGate
from .service import MqttLink, WaterService, now_ms

DATA = REPO / "data"
SIM_DIR = DATA / "sim"            # the simulator's own cards and ledger: the real ones are never touched
DEFAULTS = {"db": DATA / "water.db", "badges": REPO / registry.DEFAULT_FILE, "config": DATA / "water_point.json"}

SIM_BADGES = [("0A1B2C3D", "household", "card 7"), ("0B2C3D4E", "household", "card 8"),
              ("04112233445566", "technician", "blue key fob"), ("04A1B2C3", "operator", "white card")]
SIM_FILTERS = [("04C3A1B2E35F80", "filter 3", 500.0, 112.4), ("04C3A1B2E36080", "filter 4", 500.0, 0.0),
               ("04C3A1B2E37A90", "filter 2", 500.0, 500.0)]


def stop_signal(signum, frame):
    raise KeyboardInterrupt


def log(text):
    print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)


def resolve_uid(text, book, ledger):
    """A label ('card 7', 'filter 3') or a UID, as the contract writes it. None if neither."""
    uid = book.uid_for_label(text)
    if uid:
        return uid
    for f in ledger.filters():
        if f["label"].strip().lower() == text.strip().lower():
            return f["uid"]
    try:
        return registry.normalize_uid(text)
    except registry.BadgeError:
        return None


def set_up_sim():
    """First run of --sim: demo cards, filters and one small quota in data/sim/."""
    badges, db = SIM_DIR / "badges.json", SIM_DIR / "water.db"
    if not badges.exists():
        reg = registry.new_registry()
        for uid, role, label in SIM_BADGES:
            registry.add(reg, uid, role, label)
        registry.save(reg, badges)
    ledger = Ledger(db)
    if not ledger.filters():
        t = now_ms()
        for uid, label, capacity, litres in SIM_FILTERS:
            ledger.add_filter(uid, label, capacity, t)
            ledger.set_filter_litres(uid, litres)
        ledger.fit_filter(SIM_FILTERS[0][0], t)
        ledger.set_quota("0B2C3D4E", 2.0)  # card 8: a small quota, to see it run out
    ledger.close()
    return db, badges


SIM_HELP = """Type a command and Enter:
  tap card 7            hold a card or tag to the reader (its label or its UID)
  tds 940 | ph 5.6 | temp 14
                        what the probes read (now: safe below 500 ppm, pH 6.5 to 8.5)
  probes out | probes in
                        take the probes out of the water (no reading) or put them back
  flow 10 | flow 0      litres per minute through the open valve (0: the tank is empty)
  cards                 the cards and tags there are
  status                the water point now
  help | quit"""


def console(svc, hw, book, ledger, sim):
    """Typed commands. Returns when the person quits or the input ends."""
    while True:
        try:
            line = input()
        except EOFError:
            return False  # no terminal (a service): keep running
        words = line.strip().split()
        if not words:
            continue
        cmd, rest = words[0].lower(), " ".join(words[1:])
        if cmd in ("quit", "exit", "q"):
            return True
        if cmd == "help":
            print(SIM_HELP if sim else "tap <label or UID>, status, cards, help, quit")
        elif cmd == "tap" and rest:
            uid = resolve_uid(rest, book, ledger)
            if uid is None:
                print(f"'{rest}' is neither a known label nor a UID (type: cards)")
            else:
                hw.tap(uid)
        elif cmd == "status":
            print_status(svc.snapshot())
        elif cmd == "cards":
            book.refresh()
            for uid, b in sorted(book.by_uid.items(), key=lambda kv: kv[1].get("label", "")):
                print(f"  {b.get('label') or '-':<16} {b['role']:<11} {uid}")
            for f in ledger.filters():
                print(f"  {f['label']:<16} {'filter tag':<11} {f['uid']}  {f['litres']:.1f} of {f['capacity_l']:g} L")
        elif sim and cmd in ("tds", "ph", "temp") and rest:
            try:
                value = float(rest)
            except ValueError:
                print("a number, please")
                continue
            setattr(hw, {"tds": "tds_ppm", "ph": "ph", "temp": "temp_c"}[cmd], value)
        elif sim and cmd == "probes" and rest in ("out", "in"):
            hw.probes_in = rest == "in"
        elif sim and cmd == "flow" and rest:
            try:
                hw.flow_lpm = max(0.0, float(rest))
            except ValueError:
                print("litres per minute, please")
        else:
            print("not understood: type help")


def print_status(st):
    q, s, f = st["quality"], st["session"], st["filter"]
    print(f"  quality {q['verdict'].upper()}" + (f" ({q['reason']})" if q["reason"] else "")
          + f": TDS {q['tds_ppm']} ppm, pH {q['ph']}, {q['temp_c']} °C")
    print(f"  valve {'OPEN' if st['valve_open'] else 'closed'}"
          + (f": {s['card']}, {s['litres']:.2f} L, {s['quota_left_l']:.2f} L left today" if s else ""))
    print(f"  filter {f['tag'] or '-'}: {f['state']}"
          + (f", {f['litres']:.1f} of {f['capacity_l']:g} L" if f["litres"] is not None else ""))
    print(f"  given out today: {st['dispensed_today_l']:.2f} L")


def run(args):
    if args.sim:
        db, badges = set_up_sim()
        db = args.db or db
        badges = args.badges or badges
        hw = SimHardware(flow_lpm=args.flow)
    else:
        db, badges = args.db or DEFAULTS["db"], args.badges or DEFAULTS["badges"]
        try:
            from .hardware import PiHardware

            hw = PiHardware(PiConfig.load(args.config or DEFAULTS["config"]))
        except ImportError as e:
            print(f"The Pi's parts cannot be used here ({e}). On the Pi: pip install gpiozero smbus2 spidev "
                  "mfrc522 (and rpi-lgpio on a Pi 5). To try it on this computer: python -m station.water --sim",
                  file=sys.stderr)
            return 2
        except Exception as e:  # I2C or SPI not enabled, a pin in use...
            print(f"The Pi's parts could not be started ({type(e).__name__}: {e}). Check them one by one: "
                  "python -m station.water probe (docs/water_point.md)", file=sys.stderr)
            return 2
    ledger = Ledger(db)
    book = BadgeBook(badges)
    point = WaterPoint(ledger, book, PointSettings(quota_l=args.quota))
    svc = WaterService(point, QualityGate(Limits()), hw, MqttLink(args.broker, args.port, log), log)
    signal.signal(signal.SIGTERM, stop_signal)  # systemd's stop: the same clean stop as Ctrl-C
    svc.start()
    book.refresh()
    households = sum(1 for b in book.by_uid.values() if b["role"] == "household")
    log(f"water point {'(simulated) ' if args.sim else ''}on broker {args.broker}:{args.port}; ledger {db}")
    log(f"{households} household card(s) in {badges}; default quota {args.quota:g} L a day")
    if book.error:
        log(f"badge list not readable: {book.error}")
    f = point.filter
    log(f"filter fitted: {card_name(f['uid'], f['label']) + ', ' + format(f['litres'], '.1f') + ' L used' if f else 'none'}")
    if args.sim:
        print(SIM_HELP, flush=True)
    try:
        if not console(svc, hw, book, ledger, args.sim):
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        svc.stop()
        log("water point stopped, valve closed")
    return 0


def run_probe(args):
    try:
        from .hardware import PiHardware

        hw = PiHardware(PiConfig.load(args.config or DEFAULTS["config"]))
    except ImportError as e:
        print(f"probe runs on the Pi only ({e})", file=sys.stderr)
        return 2
    except Exception as e:
        print(f"The Pi's parts could not be started ({type(e).__name__}: {e}): are I2C and SPI enabled "
              "(sudo raspi-config), and the wiring as in docs/water_point.md?", file=sys.stderr)
        return 2
    print("Every second: probe voltages, values, flow pulses and taps. The valve stays closed. Ctrl-C to stop.")
    print("pH calibration: probe in the pH 7.00 buffer, copy the pH voltage to ph7_v in data/water_point.json;")
    print("then in the pH 4.00 buffer, to ph4_v.")
    try:
        while True:
            v_tds, v_ph = hw.probe_volts()
            r = hw.read_quality(now_ms())
            print(f"TDS {v_tds:.3f} V -> {r.tds_ppm if r.tds_ppm is None else round(r.tds_ppm)} ppm | "
                  f"pH {v_ph:.3f} V -> {r.ph if r.ph is None else round(r.ph, 2)} | "
                  f"water {r.temp_c if r.temp_c is not None else 'no DS18B20'} °C | "
                  f"flow {hw.take_flow() * hw.cfg.pulses_per_l:.0f} pulses | taps {hw.take_taps() or '-'}", flush=True)
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        hw.close()
    return 0


def admin(args):
    ledger = Ledger(args.db or DEFAULTS["db"])
    book = BadgeBook(args.badges or DEFAULTS["badges"])
    try:
        return admin_command(args, ledger, book)
    finally:
        ledger.close()


def admin_command(args, ledger, book):
    if args.command == "quota":
        uid = resolve_uid(args.card, book, ledger)
        badge = book.lookup(uid) if uid else None
        if badge is None or badge[0] != "household":
            print(f"'{args.card}' is not a household card of {book.path}: add it with "
                  f"python -m station.badges add <UID> household --label \"...\"", file=sys.stderr)
            return 2
        if args.default:
            ledger.set_quota(uid, None)
            print(f"{card_name(uid, badge[1])}: back to the default daily quota")
        elif args.litres is None or args.litres < 0:
            print("give the litres per day (0 or more), or --default", file=sys.stderr)
            return 2
        else:
            ledger.set_quota(uid, args.litres)
            print(f"{card_name(uid, badge[1])}: {args.litres:g} L a day")
    elif args.command == "quotas":
        quotas = ledger.quotas()
        if not quotas:
            print("Every household card has the default quota (python -m station.water --quota L sets it).")
        for uid, litres in quotas.items():
            badge = book.lookup(uid)
            print(f"  {card_name(uid, badge[1] if badge else ''):<16} {uid:<20} {litres:g} L a day")
    elif args.command == "filter" and args.action == "add":
        try:
            uid = registry.normalize_uid(args.uid)
        except registry.BadgeError as e:
            print(e, file=sys.stderr)
            return 2
        if book.lookup(uid) is not None:
            print(f"{uid} is a badge in {book.path}, not a filter tag", file=sys.stderr)
            return 2
        if args.capacity <= 0 or len(args.label) > 40:
            print("the capacity must be above 0 L and the label at most 40 characters", file=sys.stderr)
            return 2
        ledger.add_filter(uid, args.label or card_name(uid), args.capacity, now_ms())
        print(f"filter tag {uid} registered: {args.label or card_name(uid)}, {args.capacity:g} L. A technician "
              "fits it: their badge, then this tag, at the water point.")
    elif args.command == "filter":
        fitted = ledger.fitted_filter()
        for f in ledger.filters():
            mark = "fitted" if fitted and f["uid"] == fitted["uid"] else "used up" if f["litres"] >= f["capacity_l"] else ""
            print(f"  {f['label']:<16} {f['uid']:<20} {f['litres']:7.1f} of {f['capacity_l']:g} L  {mark}")
        if not ledger.filters():
            print("No filter tag registered: python -m station.water filter add <UID> --capacity 500 --label \"filter 1\"")
    elif args.command == "report":
        day = args.day or local_day(now_ms())
        rows = ledger.report(day)
        print(f"Water given out on {day}: {ledger.total_on(day):.2f} L")
        for uid, card, litres, sessions in rows:
            print(f"  {card:<16} {litres:7.2f} L in {sessions} session(s)")
        denied = ledger.events_on(day, "CARD_DENIED")
        if denied:
            reasons = {}
            for e in denied:
                reasons[e["data"]["reason"]] = reasons.get(e["data"]["reason"], 0) + 1
            print(f"Refused: {len(denied)} tap(s)")
            for reason, n in sorted(reasons.items(), key=lambda kv: -kv[1]):
                print(f"  {n:>3} × {reason}")
        changes = ledger.events_on(day, "QUALITY_CHANGE")
        if changes:
            print("Quality changes:")
            for e in changes:
                print(f"  {time.strftime('%H:%M:%S', time.localtime(e['ts'] / 1000))} {e['data']['verdict']}"
                      + (f": {e['data']['reason']}" if e["data"]["reason"] else ""))
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--db", help="the ledger, default data/water.db")
    common.add_argument("--badges", help="the badge list, default data/badges.json")
    if argv[:1] and argv[0] in ("quota", "quotas", "filter", "report", "probe"):
        p = argparse.ArgumentParser(prog="python -m station.water")
        sub = p.add_subparsers(dest="command", required=True)
        q = sub.add_parser("quota", parents=[common], help="a card's own daily quota")
        q.add_argument("card", help="its label ('card 7') or UID")
        q.add_argument("litres", type=float, nargs="?")
        q.add_argument("--default", action="store_true", help="back to the default quota")
        sub.add_parser("quotas", parents=[common], help="the cards with their own quota")
        f = sub.add_parser("filter", help="filter cartridge tags")
        fa = f.add_subparsers(dest="action", required=True)
        add = fa.add_parser("add", parents=[common], help="register a cartridge's tag")
        add.add_argument("uid")
        add.add_argument("--capacity", type=float, required=True, help="litres it can filter")
        add.add_argument("--label", default="", help="written on the cartridge, e.g. 'filter 3'")
        fa.add_parser("list", parents=[common])
        r = sub.add_parser("report", parents=[common], help="what was given out on a day")
        r.add_argument("--day", help="YYYY-MM-DD, default today")
        pr = sub.add_parser("probe", help="on the Pi: what each part reads")
        pr.add_argument("--config", help="default data/water_point.json")
        args = p.parse_args(argv)
        return run_probe(args) if args.command == "probe" else admin(args)
    p = argparse.ArgumentParser(prog="python -m station.water", parents=[common], description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--sim", action="store_true", help="simulated hardware, demo cards and filters in data/sim/")
    p.add_argument("--broker", default="localhost")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--quota", type=float, default=20.0, help="litres a household card may draw a day, default 20")
    p.add_argument("--flow", type=float, default=10.0, help="--sim: litres per minute through the open valve")
    p.add_argument("--config", help="the Pi's pins and calibration, default data/water_point.json")
    return run(p.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())