"""Command line for M2. Run from the project folder:

    python -m station.datalog record --tag gap15 --set gap_mm=15 --set hv_mode=ai [--duration 600]
    python -m station.datalog import run.jsonl --tag sim --set source=simulator
    python -m station.datalog build latest          (or a run id, or a run folder)
    python -m station.datalog list
"""

import argparse
import json
import shutil
import sys
from datetime import datetime

from . import dataset, reader, recorder, runs


def cmd_record(args):
    run_dir, meta = runs.new_run(args.data, args.tag, runs.parse_settings(args.set), args.notes, source="mqtt")
    print(f"Recording mqtt://{args.broker}:{args.port} into {run_dir}. Ctrl-C to stop.")
    try:
        counts = recorder.record_mqtt(run_dir / runs.RAW_FILE, args.broker, args.port, args.duration)
    except OSError as e:
        shutil.rmtree(run_dir)  # nothing was recorded: do not leave an empty run behind
        print(f"Cannot reach the MQTT broker: {e}", file=sys.stderr)
        return 2
    meta["ended"] = datetime.now().isoformat(timespec="seconds")
    meta["received"] = counts
    runs.write_meta(run_dir, meta)
    print(f"Saved {sum(counts.values())} message(s). Next: python -m station.datalog build {meta['run_id']}")
    return 0


def cmd_import(args):
    lines = [line for line in reader.read_text_lines(args.file) if line.strip().startswith("{")]
    if not lines:
        print(f"{args.file} contains no JSON lines", file=sys.stderr)
        return 2
    run_dir, meta = runs.new_run(args.data, args.tag, runs.parse_settings(args.set), args.notes, source="file")
    meta["source_file"] = str(args.file)
    runs.write_meta(run_dir, meta)
    (run_dir / runs.RAW_FILE).write_text("\n".join(line.strip() for line in lines) + "\n", encoding="utf-8")
    print(f"Imported {len(lines)} line(s) into {run_dir}. Next: python -m station.datalog build {meta['run_id']}")
    return 0


def cmd_build(args):
    run_dir = runs.resolve_run(args.run, args.data)
    summary = dataset.build(run_dir, args.config)
    stats = summary["input"]
    print(f"Run {summary['run_id']}: {stats['kept']} message(s) kept, {stats['invalid']} invalid, "
          f"{stats['unreadable']} unreadable, {stats['duplicates']} duplicate(s), {stats['skipped']} skipped")
    print("Rows: " + ", ".join(f"{name} {n}" for name, n in summary["rows"].items()))
    for node, s in summary["nodes"].items():
        print(f"  {node}: {s['duration_s']} s, water {s['mass_change_g']} g, energy {s['energy_wh']} Wh, "
              f"{s['fog_episodes']} fog episode(s), {s['arcs']} arc(s), {s['arc_warnings']} warning(s), "
              f"{s['lost_messages']} lost")
    for problem in summary["problems"][:5]:
        print(f"  problem: {problem}")
    print(f"Tables in {run_dir / runs.DATASET_DIR}")
    return 1 if stats["invalid"] or stats["unreadable"] else 0


def cmd_list(args):
    found = runs.list_runs(args.data)
    if not found:
        print(f"No runs in {args.data}")
    for run_dir in found:
        meta = runs.read_meta(run_dir)
        built = "built" if (run_dir / runs.DATASET_DIR / "summary.json").exists() else "not built"
        settings = json.dumps(meta.get("settings", {}), separators=(",", ":"))
        print(f"{meta['run_id']:40} {meta.get('source', ''):5} {built:10} {settings}")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="python -m station.datalog", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--data", default=str(runs.DEFAULT_DATA_DIR), help="runs folder, default data/runs")
    sub = p.add_subparsers(dest="command", required=True)

    def run_options(s):
        s.add_argument("--tag", default="", help="short label added to the run id, e.g. gap15")
        s.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                       help="experiment setting, repeatable: --set gap_mm=15 --set hv_mode=fixed")
        s.add_argument("--notes", default="", help="free text (no names: anonymity rule)")

    s = sub.add_parser("record", help="record live MQTT traffic into a new run")
    run_options(s)
    s.add_argument("--broker", default="localhost")
    s.add_argument("--port", type=int, default=1883)
    s.add_argument("--duration", type=float, default=None, help="stop after this many seconds")
    s.set_defaults(func=cmd_record)

    s = sub.add_parser("import", help="make a run from a JSON-lines file (simulator --out, serial_capture.py)")
    s.add_argument("file")
    run_options(s)
    s.set_defaults(func=cmd_import)

    s = sub.add_parser("build", help="build the labelled tables of a run")
    s.add_argument("run", help='run id, run folder, or "latest"')
    s.add_argument("--config", default=None, help="label settings file (default: the run's label_config.json)")
    s.set_defaults(func=cmd_build)

    s = sub.add_parser("list", help="list the runs")
    s.set_defaults(func=cmd_list)

    args = p.parse_args(argv)
    try:
        return args.func(args)
    except (ValueError, FileNotFoundError, FileExistsError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())