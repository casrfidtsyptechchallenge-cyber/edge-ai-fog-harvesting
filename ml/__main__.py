"""Command line for the models trained on the laptop. Run from the project folder (pip install -r requirements-ml.txt):

    python -m ml arc-train                train the arc predictor (M5a) on synthetic runs, test it on unseen ones,
                                          and write the firmware's arc_model_data.h
    python -m ml arc-eval                 the saved model in a closed loop: arcs per hour without and with it
    python -m ml pipeline                 M11: from the runs recorded with M2 (data/runs) to the firmware: retrain
                                          the arc predictor, check the node's models and the controller, report
    python -m ml models [--deploy NAME]   the arc predictor's versions, and the one in the firmware (or put one back)
    python -m ml demo-runs                stand-in runs (simulator + arc physics) to try the pipeline: not data
    python -m ml fog-forecast FILES.csv   fog within 1 h / 3 h from real METAR reports (IEM, Tunisia ASOS):
                                          trains on every year but the last, tests on the last
    python -m ml adapt                    experiment: on-node learning of the arc predictor's last layer from its own
                                          arcs (auto-labels, probes, federated averaging); not in the firmware
    python -m ml novelty                  the arc predictor's novelty guard: how it reacts to conditions it was
                                          not trained on, and the firmware's arc_novelty_data.h

Every model trained is kept in ml/models/ (weights, 8-bit version, rule, scores). docs/training.md: M11.
"""

import argparse
import datetime
import json
import sys
from pathlib import Path

import numpy as np

from . import arc_cnn as cnn
from . import arc_eval as ev
from . import arc_novelty as novelty
from . import fog_forecast
from . import arc_physics as phys
from . import demo_runs, pipeline

REPO = Path(__file__).resolve().parents[1]
MODEL_HEADER = REPO / "firmware" / "node" / "lib" / "edge_ai" / "arc_model_data.h"
TEST_HEADER = REPO / "firmware" / "node" / "test" / "test_edge_ai" / "arc_model_test.h"
NOVELTY_HEADER = MODEL_HEADER.parent / "arc_novelty_data.h"
MODELS = REPO / "ml" / "models"
WARN_RISK = 0.30   # the threshold the node uses (mcx_node_config.h, ARC_WARN_RISK): see arc-eval


def rounded(d, digits=3):
    return {k: (round(v, digits) if isinstance(v, float) else v) for k, v in d.items()}


def arc_train(args):
    s = args.seed * 1000
    print(f"synthetic runs: {args.train_runs} to train, {args.val_runs} to choose the epoch, "
          f"{args.test_runs} never seen to test ({args.windows} windows each)")
    X, y, _ = phys.make_dataset(args.train_runs, args.windows, s + 1)
    Xv, yv, _ = phys.make_dataset(args.val_runs, args.windows, s + 2)
    Xt, yt, mt = phys.make_dataset(args.test_runs, args.windows, s + 3)
    z, sig, med = cnn.preprocess(X)
    zv, _, _ = cnn.preprocess(Xv)
    zt, sigt, medt = cnn.preprocess(Xt)
    net = cnn.ArcNet(seed=args.seed)
    cnn.train(net, z, y, zv, yv, epochs=args.epochs, seed=args.seed)
    q = cnn.QuantArcNet(net, z[:4000])
    rule = cnn.fit_rule(z, sig, med, y)

    reports = [ev.window_report("CNN, 8 bits (as on the MCX)", q.predict(zt), yt, args.threshold),
               ev.window_report("CNN, float", net.predict(zt), yt, args.threshold),
               ev.window_report("rule-based fallback", cnn.rule_predict(zt, sigt, medt, rule), yt, args.threshold),
               ev.window_report("best possible (true probability)", mt[:, 3], yt, args.threshold)]
    print(f"\non {args.test_runs} unseen runs ({len(yt)} windows, {yt.mean():.0%} followed by an arc within "
          f"{phys.HORIZON_S:g} s):")
    print(f"{'':34} {'AUC':>6} {'log loss':>9}  warned at {args.threshold:g}: arc windows / safe windows")
    for r in reports:
        print(f"{r['name']:34} {r['auc']:6.3f} {r['log_loss']:9.4f}  {r['warned_arc_windows']:.0%} / "
              f"{r['warned_safe_windows']:.1%}")
    print("calibration of the 8-bit CNN (predicted -> observed):",
          ", ".join(f"{m:.2f}->{o:.2f}" for _, _, n, m, o in reports[0]["calibration"]))

    name = args.name or f"arc-cnn-sim-{datetime.date.today():%Y%m%d}-s{args.seed}"
    info = {"name": name, "trained": datetime.datetime.now().isoformat(timespec="seconds"), "data": "synthetic",
            "seed": args.seed, "train_runs": args.train_runs, "windows_per_run": args.windows,
            "epochs": args.epochs, "threshold": args.threshold, "tests": [rounded(dict(r, calibration=None))
                                                                         for r in reports]}
    MODELS.mkdir(parents=True, exist_ok=True)
    cnn.save_model(MODELS / f"{name}.npz", net, q, rule, info)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    notes = (f"{name}: {args.train_runs} synthetic runs (ml/arc_physics.py), AUC {reports[0]['auc']:.3f} on "
             f"{args.test_runs} unseen runs. Retrain on chamber data (M11).")
    out.write_text(cnn.header(q, rule, name, notes), encoding="utf-8")
    guard = novelty.fit_for(q, X, Xv)  # the novelty guard goes with this model, always
    gnotes = f"Fitted on its {len(X)} training windows; thresholds on {len(Xv)} validation windows."
    novelty.save(guard, novelty.sidecar(MODELS / f"{name}.npz"), {"model": name, "notes": gnotes})
    novelty.write_header(guard, name, gnotes, out.parent / "arc_novelty_data.h")
    risk, rate = q.predict(zt), cnn.pulse_rate(zt, sigt)
    picks = [int(np.argmin(risk)), int(np.argmax(risk))]          # the safest and the riskiest windows...
    for i in np.argsort(rate)[::-1]:                               # ...and the two with the most pulses
        if len(picks) == 4:
            break
        if int(i) not in picks:
            picks.append(int(i))
    picks.sort(key=lambda i: risk[i])
    Path(args.test_out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.test_out).write_text(cnn.test_header(q, Xt[picks]), encoding="utf-8")
    print(f"\nwrote {out.relative_to(REPO) if out.is_relative_to(REPO) else out}, "
          f"{Path(args.test_out).name} and ml/models/{name}.npz")
    return 0


def arc_eval(args):
    path = Path(args.model) if args.model else max(MODELS.glob("arc-*.npz"), key=lambda p: p.stat().st_mtime, default=None)
    if path is None:
        print("No model yet: python -m ml arc-train", file=sys.stderr)
        return 2
    q, rule, info = cnn.load_model(path)
    print(f"model {info['name']}: closed loop on {args.runs} unseen runs of {args.minutes} min of fog, the controller "
          "always pushing the HV up;\nwater relative to no predictor; a FAULT (3 arcs in 60 s) stops the node for 5 min")
    print(f"{'':28} {'arcs/h':>7} {'warnings/h':>11} {'faults/h':>9} {'water':>7}")
    base = ev.closed_loop(None, 1.0, args.runs, args.minutes, args.seed)
    rows = [("no predictor", base)]
    for threshold in args.thresholds:
        rows.append((f"CNN 8 bits, warn at {threshold:g}",
                     ev.closed_loop(ev.predictor_from(q, rule), threshold, args.runs, args.minutes, args.seed)))
        rows.append((f"rule-based, warn at {threshold:g}",
                     ev.closed_loop(ev.predictor_from(q, rule, False), threshold, args.runs, args.minutes, args.seed)))
    for label, r in rows:
        print(f"{label:28} {r['arcs_per_h']:7.1f} {r['warnings_per_h']:11.1f} {r['faults_per_h']:9.1f} "
              f"{r['capture'] / base['capture']:7.0%}")
    result = {"model": info["name"], "runs": args.runs, "minutes": args.minutes,
              "rows": [dict(rounded(r), label=label) for label, r in rows]}
    (MODELS / f"{info['name']}.eval.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0


def novelty_cmd(args):
    name = args.model or pipeline.deployed_name(MODEL_HEADER)
    path = Path(name) if str(name).endswith(".npz") else MODELS / f"{name}.npz"
    report = MODELS / f"{path.stem}.novelty-eval.json"
    guard, rows = novelty.run(path, args.out, report)
    print(f"novelty guard of {path.stem}: shape threshold d² = {guard.threshold:.1f}")
    print(f"{'condition':42} {'novel':>6}  {'AUC CNN':>8} {'AUC rule':>9}   why: saturated / out of range / shape")
    for r in rows:
        print(f"{r['condition']:42} {r['novel']:6.1%}  {r['auc_cnn']:8.3f} {r['auc_rule']:9.3f}   "
              f"{r['saturated']:.0%} / {r['out_of_range']:.0%} / {r['shape']:.0%}")
    print(f"\nwrote {Path(args.out).name} and {report.relative_to(REPO)}")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="python -m ml", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    t = sub.add_parser("arc-train", help="train the arc predictor and write arc_model_data.h")
    t.add_argument("--train-runs", type=int, default=60)
    t.add_argument("--val-runs", type=int, default=15)
    t.add_argument("--test-runs", type=int, default=25)
    t.add_argument("--windows", type=int, default=400, help="windows per run")
    t.add_argument("--epochs", type=int, default=15)
    t.add_argument("--seed", type=int, default=1)
    t.add_argument("--threshold", type=float, default=WARN_RISK)
    t.add_argument("--name", help="model name, default arc-cnn-sim-<date>-s<seed>")
    t.add_argument("--out", default=str(MODEL_HEADER))
    t.add_argument("--test-out", default=str(TEST_HEADER))
    e = sub.add_parser("arc-eval", help="arcs per hour in a closed loop, without and with the predictor")
    e.add_argument("--model", help="a .npz in ml/models, default the newest")
    e.add_argument("--runs", type=int, default=8)
    e.add_argument("--minutes", type=int, default=30)
    e.add_argument("--seed", type=int, default=100)
    e.add_argument("--thresholds", type=float, nargs="+", default=[0.7, 0.5, 0.3])
    pl = sub.add_parser("pipeline", help="M11: retrain from the recorded runs, check the models, write a report")
    pl.add_argument("runs", nargs="*", help="run ids or folders (default: every run in --data)")
    pl.add_argument("--data", default=str(pipeline.DATA), help="the runs folder, default data/runs")
    pl.add_argument("--synthetic-runs", type=int, default=30, help="synthetic runs trained with the recorded ones")
    pl.add_argument("--epochs", type=int, default=12)
    pl.add_argument("--seed", type=int, default=1)
    pl.add_argument("--threshold", type=float, default=WARN_RISK)
    pl.add_argument("--min-test", type=int, default=50, help="fewest test windows to judge the new model on")
    pl.add_argument("--compare-by", default="controller", help="the run setting the controller table groups by")
    pl.add_argument("--force", action="store_true", help="write the firmware's headers even if not better")
    pl.add_argument("--dry-run", action="store_true", help="train, test and report; change no firmware file")
    pl.add_argument("--name", help="model name, default arc-cnn-runs-<date>-<time>")
    pl.add_argument("--out", default=str(MODEL_HEADER))
    pl.add_argument("--test-out", default=str(TEST_HEADER))
    pl.add_argument("--reports", default=str(pipeline.REPORTS), help="where the report goes, default data/reports")
    pl.add_argument("--models", default=str(MODELS), help="where the versions are kept, default ml/models")
    mo = sub.add_parser("models", help="the arc predictor's versions kept in ml/models, and the one in the firmware")
    mo.add_argument("--deploy", metavar="NAME", help="write the firmware's headers from this saved version")
    d = sub.add_parser("demo-runs", help="stand-in runs to try the pipeline (NOT measurements), in data/demo-runs")
    d.add_argument("--count", type=int, default=4)
    d.add_argument("--minutes", type=int, default=30)
    d.add_argument("--seed", type=int, default=1)
    d.add_argument("--data", default=str(demo_runs.DEMO))
    nv = sub.add_parser("novelty", help="the arc predictor's novelty guard: evaluation and arc_novelty_data.h")
    nv.add_argument("--model", help="a .npz in ml/models, default the one in the firmware")
    nv.add_argument("--out", default=str(NOVELTY_HEADER))
    ff = sub.add_parser("fog-forecast", help="fog within 1 h / 3 h from real METAR reports (IEM Tunisia ASOS CSV)")
    ff.add_argument("files", nargs="+", help="IEM ASOS CSV files (tmpc, dwpc, relh, sknt, drct, vsby; UTC)")
    ff.add_argument("--out", default=str(MODELS / "fog-forecast.json"), help="report and model coefficients")
    ad = sub.add_parser("adapt", help="experiment: on-node learning of the last layer (not in the firmware)")
    ad.add_argument("--seeds", type=int, nargs="+", default=[0, 1, 2, 3, 4])
    ad.add_argument("--eval-minutes", type=int, default=30)
    args = p.parse_args(argv)
    if args.command == "adapt":
        from . import arc_adapt
        name = pipeline.deployed_name(MODEL_HEADER)
        q, _, info = cnn.load_model(MODELS / f"{name}.npz")
        s0 = info.get("seed", 1) * 1000
        Xs, _, _ = phys.make_dataset(20, 200, s0 + 1)
        rows = arc_adapt.experiment(q, arc_adapt.feature_scale(q, Xs), args.seeds, eval_min=args.eval_minutes)
        out = MODELS / "arc-adapt-experiment.json"
        out.write_text(json.dumps(rows, indent=1, default=float), encoding="utf-8")
        print(f"wrote {out.relative_to(REPO)}")
        return 0
    if args.command == "fog-forecast":
        fog_forecast.run(args.files, args.out)
        print(f"\nwrote {args.out}")
        return 0
    if args.command == "novelty":
        return novelty_cmd(args)
    if args.command == "pipeline":
        path, code = pipeline.run(args)
        if path is not None:
            print("\n" + path.read_text(encoding="utf-8"))
            print(f"report: {path.relative_to(REPO) if path.is_relative_to(REPO) else path}")
        return code
    if args.command == "models":
        return pipeline.deploy(args.deploy) if args.deploy else pipeline.list_models()
    if args.command == "demo-runs":
        print(f"stand-in runs (simulator + arc physics, not measurements) in {args.data}:")
        demo_runs.make_runs(args.data, args.count, args.minutes, args.seed)
        print(f"next: python -m ml pipeline --data {args.data}")
        return 0
    return arc_train(args) if args.command == "arc-train" else arc_eval(args)


if __name__ == "__main__":
    sys.exit(main())