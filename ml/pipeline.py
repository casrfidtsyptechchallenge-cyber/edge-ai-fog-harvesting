"""The training pipeline (M11): from the runs recorded with M2 to the firmware, in one command.

    python -m ml pipeline                  every run in data/runs
    python -m ml pipeline RUN RUN ...      only these runs (ids or folders)
    python -m ml models                    the arc predictor's versions, and the one in the firmware
    python -m ml models --deploy NAME      put a saved version back into the firmware's headers

1. Datasets: each run's tables are built again when its raw.jsonl is newer (as python -m station.datalog build).
2. Arc predictor (M5a): trained on the recorded windows (wave messages: `waves 2`, warnings) together with
   synthetic ones (ml/arc_physics.py), so a few hours of chamber data adapt the model instead of starting it
   from nothing. Tested on the newest runs, which it did not train on, against the model now in the firmware.
   The firmware's headers are written only if the new model does better there (lower log loss), or with --force.
3. Virtual fog sensor (M5b) and electrode health (M5c) learn on the node: they are checked on the runs
   (virtual against optical fog; the onset per run).
4. HV controller (M6): water, grams per Wh and arcs per hour for each value of a run setting
   (record with --set controller=m6, or controller=fixed-0.60, ...).
5. A report: data/reports/pipeline-<date>.md.
"""

import csv
import datetime
import json
import math
import re
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

from . import arc_cnn as cnn
from . import arc_novelty as novelty
from . import arc_eval as ev
from . import arc_physics as phys

REPO = Path(__file__).resolve().parents[1]
DATA = REPO / "data" / "runs"
REPORTS = REPO / "data" / "reports"
MODELS = REPO / "ml" / "models"
MODEL_HEADER = REPO / "firmware" / "node" / "lib" / "edge_ai" / "arc_model_data.h"
TEST_HEADER = REPO / "firmware" / "node" / "test" / "test_edge_ai" / "arc_model_test.h"
FS_TOLERANCE = 0.10      # the CNN takes windows within 10 % of its sample rate (as arc_predictor.cpp)
TEST_SHARE = 0.25        # the newest quarter of the runs is kept for the test
MIN_CLASS = 5            # the test needs at least this many windows followed by an arc, and as many not


# ---------------------------------------------------------------- runs and their tables

def run_dirs(data_dir, names=None):
    from station.datalog import runs

    if names:
        return [runs.resolve_run(name, data_dir) for name in names]
    return runs.list_runs(data_dir)


def ensure_datasets(dirs, log=print):
    """Builds the tables of the runs that have none, or whose raw.jsonl changed since."""
    from station.datalog import dataset, runs

    for d in dirs:
        summary, raw = d / runs.DATASET_DIR / "summary.json", d / runs.RAW_FILE
        if not summary.exists() or (raw.exists() and raw.stat().st_mtime > summary.stat().st_mtime):
            dataset.build(d)
            log(f"  {d.name}: tables built")


def read_table(run_dir, name):
    path = Path(run_dir) / "dataset" / name
    if not path.exists():
        return []
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def settings_of(run_dir):
    try:
        return json.loads((Path(run_dir) / "run.json").read_text(encoding="utf-8")).get("settings", {})
    except (OSError, ValueError):
        return {}


def number(text):
    try:
        v = float(text)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


# ---------------------------------------------------------------- arc predictor (M5a)

def load_windows(dirs):
    """The recorded windows the CNN can learn from: 256 samples near 10 kHz. Not the ARC windows: they hold
    the arc itself, not what came before it. Returns a dict of arrays (X, y, run, ts) and what was skipped."""
    X, y, run, ts = [], [], [], []
    skipped = Counter()
    for i, d in enumerate(dirs):
        for row in read_table(d, "windows.csv"):
            if row["trigger"] == "ARC":
                skipped["ARC windows (the arc itself)"] += 1
                continue
            # A back-off on a prediction followed: whether an arc would have come is unknown. Keeping these
            # windows with arc_followed = 0 would teach the model that its warnings were wrong (intervention
            # bias). Older datasets have no "intervened" column: an ARC_PREDICTED window always had a back-off.
            intervened = row.get("intervened")
            if (intervened == "1" if intervened not in (None, "") else row["trigger"] == "ARC_PREDICTED"):
                skipped["a back-off followed (outcome unknown)"] += 1
                continue
            samples = row["samples"].split()
            fs = number(row["fs_hz"]) or 0.0
            if len(samples) != phys.WINDOW or abs(fs - phys.FS_HZ) > FS_TOLERANCE * phys.FS_HZ:
                skipped["another length or sample rate"] += 1
                continue
            X.append(np.array(samples, dtype=np.float32))
            y.append(int(row["arc_followed"]))
            run.append(i)
            ts.append(int(row["ts"]))
    windows = {"X": np.array(X, dtype=np.float32).reshape(-1, phys.WINDOW), "y": np.array(y, dtype=np.int8),
               "run": np.array(run, dtype=int), "ts": np.array(ts, dtype=np.int64)}
    return windows, skipped


def split(windows, test_share=TEST_SHARE):
    """Test on the newest runs (folders sort by date). With a single run: its last quarter, in time."""
    run, ts = windows["run"], windows["ts"]
    present = sorted(set(run.tolist()))
    if len(present) >= 2:
        test_runs = set(present[-max(1, round(len(present) * test_share)):])
        test = np.array([r in test_runs for r in run], dtype=bool)
    else:
        test = ts > np.quantile(ts, 1 - test_share) if len(ts) else np.zeros(0, dtype=bool)
    return ~test, test


def deployed_name(header=MODEL_HEADER):
    try:
        found = re.search(r'#define ARC_MODEL_NAME "([^"]+)"', Path(header).read_text(encoding="utf-8"))
    except OSError:
        return None
    return found.group(1) if found else None


def predict_both(q, rule, X):
    z, sigma, median = cnn.preprocess(X)
    return q.predict(z), cnn.rule_predict(z, sigma, median, rule)


def train_arc(X, y, synthetic_runs, epochs, seed, log=print):
    """The recorded windows (the last fifth by time for validation) plus synthetic runs."""
    order = np.arange(len(X))
    n_val = len(X) // 5 if len(X) >= 150 else 0
    real_train, real_val = order[:len(X) - n_val], order[len(X) - n_val:]
    Xs, ys, _ = phys.make_dataset(synthetic_runs, 400, seed * 1000 + 11) if synthetic_runs else (
        np.zeros((0, phys.WINDOW), np.float32), np.zeros(0, np.int8), None)
    Xsv, ysv, _ = phys.make_dataset(max(2, synthetic_runs // 6), 400, seed * 1000 + 12) if synthetic_runs else (
        np.zeros((0, phys.WINDOW), np.float32), np.zeros(0, np.int8), None)
    # The recorded windows weigh as much as the synthetic ones together (repeated, up to 20 times).
    repeat = int(min(20, max(1, round(len(Xs) / max(1, len(real_train)))))) if len(real_train) else 0
    Xt = np.concatenate([Xs, np.repeat(X[real_train], repeat, axis=0)])
    yt = np.concatenate([ys, np.repeat(y[real_train], repeat)])
    Xv = np.concatenate([Xsv, X[real_val]])
    yv = np.concatenate([ysv, y[real_val]])
    z, sigma, median = cnn.preprocess(Xt)
    zv, _, _ = cnn.preprocess(Xv)
    log(f"  training on {len(real_train)} recorded windows (x{repeat}) + {len(Xs)} synthetic; "
        f"validation {len(real_val)} recorded + {len(Xsv)} synthetic")
    net = cnn.ArcNet(seed=seed)
    cnn.train(net, z, yt, zv, yv, epochs=epochs, seed=seed, log=lambda text: log("    " + text))
    mix = np.random.default_rng(seed).permutation(len(z))[:4000]
    q = cnn.QuantArcNet(net, z[mix])
    rule = cnn.fit_rule(z, sigma, median, yt)
    return net, q, rule


def golden_windows(q, X_real, seed):
    """Windows for the firmware's golden test: the safest and the riskiest, and the two with the most pulses,
    from the recorded test windows and a few synthetic ones (so very safe and very risky ones are there)."""
    Xs, _, _ = phys.make_dataset(4, 100, seed * 1000 + 13)
    X = np.concatenate([Xs, X_real]) if len(X_real) else Xs
    z, sigma, _ = cnn.preprocess(X)
    risk, rate = q.predict(z), cnn.pulse_rate(z, sigma)
    picks = [int(np.argmin(risk)), int(np.argmax(risk))]
    for i in np.argsort(rate)[::-1]:
        if len(picks) == 4:
            break
        if int(i) not in picks:
            picks.append(int(i))
    picks.sort(key=lambda i: risk[i])
    return X[picks]


def arc_step(dirs, args, log=print):
    """Returns the section of the report about the arc predictor, and whether the headers were written."""
    windows, skipped = load_windows(dirs)
    X, y = windows["X"], windows["y"]
    lines = ["## Arc predictor (M5a)", ""]
    skipped_text = ", ".join(f"{n} {why}" for why, n in skipped.items()) or "none"
    lines.append(f"Recorded windows: {len(X)} usable, {int(y.sum()) if len(y) else 0} followed by an arc within "
                 f"{phys.HORIZON_S:g} s; skipped: {skipped_text}.")
    if len(X) == 0:
        lines += ["", "No recorded windows: nothing to train on. Record some (`waves 2` on the MCX while M2 "
                      "records, docs/edge_ai.md); the firmware keeps its model."]
        return lines, False
    train, test = split(windows)
    log(f"arc predictor: {len(X)} recorded windows, {int(train.sum())} to train, {int(test.sum())} to test")
    net, q, rule = train_arc(X[train], y[train], args.synthetic_runs, args.epochs, args.seed, log)

    yt = y[test]
    judgeable = len(yt) >= args.min_test and MIN_CLASS <= yt.sum() <= len(yt) - MIN_CLASS
    reports = []
    if len(yt):
        new_cnn, new_rule = predict_both(q, rule, X[test])
        reports += [ev.window_report("new CNN, 8 bits", new_cnn, yt, args.threshold),
                    ev.window_report("new rule-based fallback", new_rule, yt, args.threshold)]
    old_name = deployed_name(args.out)
    models = Path(args.models)
    old_path = models / f"{old_name}.npz" if old_name else None
    old = None
    if old_path is not None and old_path.exists() and len(yt):
        oq, orule, _ = cnn.load_model(old_path)
        old_cnn, old_rule = predict_both(oq, orule, X[test])
        old = ev.window_report(f"firmware's CNN ({old_name})", old_cnn, yt, args.threshold)
        reports += [old, ev.window_report("firmware's rule-based fallback", old_rule, yt, args.threshold)]

    lines += ["", f"Tested on {len(yt)} windows of the newest run(s), never trained on "
                  f"({int(yt.sum()) if len(yt) else 0} followed by an arc):", "",
              f"| | AUC | log loss | arc windows warned at {args.threshold:g} | safe windows warned |",
              "|---|---|---|---|---|"]
    for r in reports:
        fmt = lambda v, f: "-" if v is None or (isinstance(v, float) and math.isnan(v)) else format(v, f)
        lines.append(f"| {r['name']} | {fmt(r['auc'], '.3f')} | {fmt(r['log_loss'], '.4f')} | "
                     f"{fmt(r['warned_arc_windows'], '.0%')} | {fmt(r['warned_safe_windows'], '.1%')} |")

    better = old is None or reports[0]["log_loss"] < old["log_loss"]
    if not judgeable:
        decision = (f"too few test windows to judge (need {args.min_test}, with at least {MIN_CLASS} followed by an "
                    "arc and as many not)")
        write = args.force
    elif better:
        decision = "the new model does better on the test runs" if old else "no model to compare with"
        write = True
    else:
        decision = "the firmware's model does as well or better on the test runs"
        write = args.force
    if args.dry_run:
        write = False

    name = args.name or f"arc-cnn-runs-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    info = {"name": name, "trained": datetime.datetime.now().isoformat(timespec="seconds"),
            "data": f"{len(dirs)} recorded run(s) + {args.synthetic_runs} synthetic",
            "runs": [d.name for d in dirs], "windows": int(len(X)), "test_windows": int(len(yt)),
            "seed": args.seed, "epochs": args.epochs, "threshold": args.threshold,
            "compared_with": old_name, "decision": decision, "exported": write,
            "tests": [{k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items() if k != "calibration"}
                      for r in reports]}
    models.mkdir(parents=True, exist_ok=True)
    cnn.save_model(models / f"{name}.npz", net, q, rule, info)
    # The novelty guard of this model: fitted on what it was trained on (recorded + synthetic windows),
    # thresholds on windows it was not (every 5th recorded training window is held out for that).
    Xs, _, _ = phys.make_dataset(args.synthetic_runs, 400, args.seed * 1000 + 11) if args.synthetic_runs else (
        np.zeros((0, phys.WINDOW), np.float32), None, None)
    Xsv, _, _ = phys.make_dataset(max(2, args.synthetic_runs // 6), 400, args.seed * 1000 + 12) if (
        args.synthetic_runs) else (np.zeros((0, phys.WINDOW), np.float32), None, None)
    tr_idx = np.flatnonzero(train)
    hold = np.zeros(len(tr_idx), bool)
    hold[::5] = True
    guard = novelty.fit_for(q, np.concatenate([X[tr_idx[~hold]], Xs]), np.concatenate([X[tr_idx[hold]], Xsv]))
    gnotes = f"Fitted on {int((~hold).sum()) + len(Xs)} training windows (recorded + synthetic)."
    novelty.save(guard, novelty.sidecar(models / f"{name}.npz"), {"model": name, "notes": gnotes})
    lines += ["", f"Kept as {models.name}/{name}.npz. Decision: {decision}."]
    if write:
        auc = reports[0]["auc"] if reports else float("nan")
        notes = f"{name}: {len(dirs)} recorded run(s) + {args.synthetic_runs} synthetic, AUC {auc:.3f} on the test runs."
        text = cnn.header(q, rule, name, notes).replace("python -m ml arc-train: do not edit, train again",
                                                        "python -m ml pipeline: do not edit, run it again")
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(text, encoding="utf-8")
        golden = cnn.test_header(q, golden_windows(q, X[test], args.seed))
        novelty.write_header(guard, name, gnotes, Path(args.out).parent / "arc_novelty_data.h")
        Path(args.test_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.test_out).write_text(golden.replace("python -m ml arc-train", "python -m ml pipeline"),
                                       encoding="utf-8")
        lines += ["", "**The firmware's headers were written** (arc_model_data.h, arc_model_test.h). Next: "
                      "`pio test -e native -f test_edge_ai`, then build and flash the MCX (docs/mcx_node.md)."]
    else:
        lines += ["", "The firmware's headers were not changed" + (" (--dry-run)." if args.dry_run else ".")]
    return lines, write


# ---------------------------------------------------------------- virtual fog (M5b), electrode health (M5c)

def node_models_step(dirs):
    lines = ["## Virtual fog sensor (M5b) and electrode health (M5c)", "",
             "Both learn on the node; here they are checked on what the runs recorded.", "",
             "| run | node | HV-on rows | virtual fog given | its error against the optical sensor | bias | "
             "onset first → last | health score (last) |", "|---|---|---|---|---|---|---|---|"]
    any_row = False
    for d in dirs:
        per_node = defaultdict(list)
        for row in read_table(d, "telemetry.csv"):
            if row.get("hv_on") == "1":
                per_node[row["node_id"]].append(row)
        for node, rows in sorted(per_node.items()):
            any_row = True
            pairs = [(number(r["fog_idx_virt"]), number(r["fog_idx_opt"])) for r in rows]
            both = [(v, o) for v, o in pairs if v is not None and o is not None]
            given = sum(1 for v, _ in pairs if v is not None)
            mae = sum(abs(v - o) for v, o in both) / len(both) if both else None
            bias = sum(v - o for v, o in both) / len(both) if both else None
            onsets = [number(r.get("health_onset_level")) for r in rows]
            onsets = [o for o in onsets if o is not None]
            scores = [number(r.get("health_score")) for r in rows]
            scores = [s for s in scores if s is not None]
            lines.append(
                f"| {d.name} | {node} | {len(rows)} | {given / len(rows):.0%} | "
                f"{'-' if mae is None else f'{mae:.3f}'} | {'-' if bias is None else f'{bias:+.3f}'} | "
                f"{f'{onsets[0]:.3f} → {onsets[-1]:.3f}' if onsets else '-'} | "
                f"{f'{scores[-1]:.2f}' if scores else '-'} |")
    if not any_row:
        lines.append("| (no telemetry with the HV on) | | | | | | | |")
    lines += ["", "Error: mean absolute difference between fog_idx_virt and fog_idx_opt while both are known "
                  "(fog index 0 to 1). A rising onset with a falling score means a fouling electrode."]
    return lines


# ---------------------------------------------------------------- HV controller (M6)

def controller_step(dirs, key):
    groups = defaultdict(lambda: {"runs": 0, "fog_s": 0.0, "water": 0.0, "energy": 0.0, "fog_weighted": 0.0,
                                  "arcs": 0, "warnings": 0, "faults": 0, "run_s": 0.0})
    for d in dirs:
        value = settings_of(d).get(key, "(not set)")
        g = groups[str(value)]
        g["runs"] += 1
        for s in read_table(d, "segments.csv"):
            duration = number(s["duration_s"]) or 0.0
            g["fog_s"] += duration
            g["water"] += number(s["mass_gain_g"]) or 0.0
            g["energy"] += number(s["energy_wh"]) or 0.0
            g["fog_weighted"] += (number(s["mean_fog_idx"]) or 0.0) * duration
            g["arcs"] += int(s["arcs"] or 0)
            g["warnings"] += int(s["arc_warnings"] or 0)
        events = read_table(d, "events.csv")
        g["faults"] += sum(1 for e in events if e["type"] == "FAULT")
        try:
            summary = json.loads((d / "dataset" / "summary.json").read_text(encoding="utf-8"))
            g["run_s"] += max((n.get("duration_s") or 0) for n in summary["nodes"].values()) if summary["nodes"] else 0
        except (OSError, ValueError, KeyError):
            pass
    lines = ["## HV controller (M6): per value of the run setting `" + key + "`", "",
             "From the fog episodes (segments.csv). Compare like with like: the same fog, the same electrode.", "",
             "| " + key + " | runs | hours of fog | mean fog | water g/h | g/Wh (HV energy) | arcs/h | warnings/h "
             "| faults per run hour |", "|---|---|---|---|---|---|---|---|---|"]
    for value, g in sorted(groups.items()):
        hours = g["fog_s"] / 3600
        per_hour = lambda n: f"{n / hours:.1f}" if hours > 0 else "-"
        fog = f"{g['fog_weighted'] / g['fog_s']:.2f}" if g["fog_s"] > 0 else "-"
        g_per_wh = f"{g['water'] / g['energy']:.1f}" if g["energy"] > 0 else "-"
        faults = f"{g['faults'] / (g['run_s'] / 3600):.2f}" if g["run_s"] > 0 else "-"
        lines.append(f"| {value} | {g['runs']} | {hours:.2f} | {fog} | {per_hour(g['water'])} | {g_per_wh} | "
                     f"{per_hour(g['arcs'])} | {per_hour(g['warnings'])} | {faults} |")
    if not groups:
        lines.append("| (no runs) | | | | | | | | |")
    lines += ["", "To compare M6 with fixed levels, record runs with `--set " + key + "=m6` and `--set " + key +
              "=fixed-0.60` (MCX default, `m6 off`), in the same fog."]
    return lines


# ---------------------------------------------------------------- the whole pipeline

def run(args, log=print):
    dirs = run_dirs(args.data, args.runs)
    if not dirs:
        log(f"No runs in {args.data}: record some first (python -m station.datalog record ...).")
        return None, 2
    log(f"{len(dirs)} run(s): " + ", ".join(d.name for d in dirs))
    ensure_datasets(dirs, log)
    report = [f"# Training pipeline (M11), {datetime.datetime.now():%Y-%m-%d %H:%M}", "",
              f"Runs ({len(dirs)}): " + ", ".join(f"`{d.name}`" for d in dirs), ""]
    demo = [d.name for d in dirs if settings_of(d).get("source") == "demo"]
    if demo:
        report += [f"**{len(demo)} of these are stand-in runs (python -m ml demo-runs), not measurements.** They show "
                   "that the pipeline works. Their arcs come from the stand-in windows, whatever the level, so "
                   "the controller table's arcs per hour mean nothing.", ""]
    arc_lines, _ = arc_step(dirs, args, log)
    report += arc_lines + [""] + node_models_step(dirs) + [""] + controller_step(dirs, args.compare_by) + [""]
    text = "\n".join(report)
    out_dir = Path(args.reports)
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"pipeline-{datetime.datetime.now():%Y%m%d-%H%M%S}.md"
    path.write_text(text, encoding="utf-8")
    return path, 0


def list_models(models=MODELS, header=MODEL_HEADER, log=print):
    deployed = deployed_name(header)
    paths = sorted(Path(models).glob("arc-*.npz"), key=lambda p: p.stat().st_mtime)
    if not paths:
        log("No model yet: python -m ml arc-train (synthetic) or python -m ml pipeline (recorded runs)")
        return 2
    log(f"{'model':34} {'trained':20} {'AUC':>6} {'log loss':>9}  data")
    for p in paths:
        _, _, info = cnn.load_model(p)
        test = (info.get("tests") or [{}])[0]
        auc, loss = test.get("auc"), test.get("log_loss")
        mark = "   <- in the firmware" if info.get("name") == deployed else ""
        log(f"{info.get('name', p.stem):34} {info.get('trained', '?'):20} "
            f"{'-' if auc is None else f'{auc:6.3f}':>6} {'-' if loss is None else f'{loss:9.4f}':>9}  "
            f"{info.get('data', '?')}{mark}")
    return 0


def deploy(name, models=MODELS, out=MODEL_HEADER, test_out=TEST_HEADER, seed=1, log=print):
    """Writes the firmware's headers from a saved version (to go back to an earlier model)."""
    path = Path(models) / f"{name}.npz"
    if not path.exists():
        log(f"No {path.name} in {models}: see python -m ml models")
        return 2
    q, rule, info = cnn.load_model(path)
    notes = f"{name}: deployed again from {Path(models).name}/{path.name} ({info.get('data', '?')})."
    text = cnn.header(q, rule, name, notes).replace("python -m ml arc-train: do not edit, train again",
                                                    "python -m ml models --deploy: do not edit")
    Path(out).write_text(text, encoding="utf-8")
    golden = cnn.test_header(q, golden_windows(q, np.zeros((0, phys.WINDOW), np.float32), seed))
    Path(test_out).write_text(golden.replace("python -m ml arc-train", "python -m ml models --deploy"),
                              encoding="utf-8")
    _, _, _, guard, gnotes = novelty.for_saved_model(path, log)  # the novelty guard goes with the model
    novelty.write_header(guard, name, gnotes, Path(out).parent / "arc_novelty_data.h")
    log(f"{name} is in the firmware's headers. Next: pio test -e native -f test_edge_ai, then build and flash "
        "the MCX.")
    return 0