"""How good the tracker is (M8): a simulator run recorded with --truth, replayed through the tracker.

    python sim/fake_node.py --out run.jsonl --no-wait --duration 7200 --truth --seed 1
    python -m station.network score run.jsonl

For every fog front the simulator made, it compares:
- direction and speed: the tracker's last estimate against the truth;
- arrival: when the tracker expected the fog at a node, against when the node detected it;
- pre-arming: was each node reached after the first one pre-armed in time (its PREARM window, sent before
  the fog came, still open when the fog came), and how long before.

Open loop: the PREARMs are counted but not delivered (the recorded nodes already ran), so the detection
times are those of nodes that were not pre-armed. A pre-armed node detects a little earlier (lower
threshold), which only helps.
"""

import json
import statistics
from pathlib import Path

import aeroion_contract as ac

from .engine import NetworkEngine
from .front import angle_error
from .tracker import PREARM_WINDOW_S, FrontTracker, TrackerSettings


def read_rows(path):
    data = Path(path).read_bytes()
    text = data.decode("utf-16") if data.startswith((b"\xff\xfe", b"\xfe\xff")) else data.decode("utf-8-sig")
    for line in text.splitlines():
        line = line.strip().lstrip("\ufeff")
        if not line.startswith("{"):
            continue
        try:
            row = json.loads(line)
            yield row["topic"], row["payload"]
        except (ValueError, KeyError, TypeError):
            continue


def replay(path, tracker):
    """Feeds a recorded run to the tracker on the run's own clock. Returns the true fronts (simulator)."""
    engine = NetworkEngine(tracker)
    truths, now, last_tick = [], 0, 0
    for topic, payload in read_rows(path):
        ts = payload.get("ts") if isinstance(payload, dict) else None
        if isinstance(ts, int):
            now = max(now, ts)
        if topic == f"{ac.TOPIC_ROOT}/{ac.SIM}/truth":
            truths.append(payload)
            continue
        if now - last_tick >= 1000:
            engine.tick(now)
            last_tick = now
        engine.handle(topic, payload, now)
    engine.tick(now + 3_600_000)  # closes the last front
    return truths


def score_run(path, tracker=None):
    tracker = tracker or FrontTracker(TrackerSettings(keep_finished=10**6))
    truths = replay(path, tracker)
    rows, eta_errors, leads = [], [], []
    later_nodes = in_time = not_armed = 0
    for f in tracker.finished:
        truth = max((t for t in truths if t["ts"] <= f.started_ms), key=lambda t: t["ts"], default=None)
        if truth is None:
            continue
        fit = f.fit
        row = {"start_ms": f.started_ms, "true_dir": truth["direction_deg"], "true_speed": truth["speed_mps"],
               "nodes": len(f.detections), "basis": fit.basis if fit else "none", "dir": None, "speed": None,
               "dir_err": None, "speed_err_pct": None, "prearmed": 0, "later": 0, "not_armed": 0}
        if fit is not None and fit.direction_deg is not None:
            row.update(dir=round(fit.direction_deg, 1), speed=round(fit.speed_mps, 2),
                       dir_err=round(angle_error(fit.direction_deg, truth["direction_deg"]), 1),
                       speed_err_pct=round(100 * (fit.speed_mps - truth["speed_mps"]) / truth["speed_mps"], 1))
        order = list(f.detections)
        for node in order[1:]:
            seen = f.detections[node]
            sent = [(ms, eta) for ms, eta in f.prearms.get(node, []) if ms <= seen]
            if not sent and node in f.not_armed:
                row["not_armed"] += 1  # LOCKED, SAFE...: no PREARM can help it
                continue
            row["later"] += 1
            if sent and seen <= sent[-1][0] + (sent[-1][1] + PREARM_WINDOW_S) * 1000:
                row["prearmed"] += 1
                leads.append((seen - sent[0][0]) / 1000)
            stages = set()  # one prediction per stage (after the 1st detection, the 2nd...), not one per second
            for now_ms, n_det, basis, direction, _speed, expected in f.estimates:
                if now_ms < seen and basis != "distance" and direction is not None and node in expected \
                        and n_det not in stages:
                    stages.add(n_det)
                    eta_errors.append(abs(expected[node] - seen) / 1000)
        later_nodes += row["later"]
        in_time += row["prearmed"]
        not_armed += row["not_armed"]
        rows.append(row)
    fitted = [r for r in rows if r["basis"] == "fit" and r["dir_err"] is not None]

    def median(values):
        return round(statistics.median(values), 1) if values else None

    return {
        "fronts_true": len(truths), "fronts_tracked": len(rows), "rows": rows,
        "fitted": len(fitted),
        "median_dir_err_deg": median([r["dir_err"] for r in fitted]),
        "median_speed_err_pct": median([abs(r["speed_err_pct"]) for r in fitted]),
        "later_nodes": later_nodes, "prearmed_in_time": in_time, "not_armed": not_armed,
        "median_lead_s": median(leads), "median_eta_err_s": median(eta_errors), "predictions": len(eta_errors),
        "prior": tracker.prior,
    }


def format_report(r):
    lines = [f"{'start':>8} {'true dir':>8} {'speed':>6} | {'nodes':>5} {'basis':>8} {'dir':>6} {'err':>5} "
             f"{'speed':>6} {'err %':>6} | pre-armed in time"]
    t0 = r["rows"][0]["start_ms"] if r["rows"] else 0
    for row in r["rows"]:
        def cell(v, w):
            return f"{'–' if v is None else v:>{w}}"
        lines.append(f"{(row['start_ms'] - t0) / 1000:>7.0f}s {row['true_dir']:>8.1f} {row['true_speed']:>6.2f} | "
                     f"{row['nodes']:>5} {row['basis']:>8} {cell(row['dir'], 6)} {cell(row['dir_err'], 5)} "
                     f"{cell(row['speed'], 6)} {cell(row['speed_err_pct'], 6)} | {row['prearmed']} of {row['later']}"
                     + (f" ({row['not_armed']} not ARMED)" if row["not_armed"] else ""))
    lines.append("")
    lines.append(f"fronts: {r['fronts_tracked']} tracked of {r['fronts_true']} made by the simulator; "
                 f"{r['fitted']} measured on 3+ nodes")
    lines.append(f"direction error (median, measured fronts): {r['median_dir_err_deg']}°   "
                 f"speed error: {r['median_speed_err_pct']} %")
    pct = round(100 * r["prearmed_in_time"] / r["later_nodes"]) if r["later_nodes"] else None
    lines.append(f"nodes pre-armed in time: {r['prearmed_in_time']} of {r['later_nodes']} reached after the first"
                 + (f" ({pct} %)" if pct is not None else "")
                 + f", median warning {r['median_lead_s']} s before the fog")
    if r["not_armed"]:
        lines.append(f"  and {r['not_armed']} reached while not ARMED (LOCKED or SAFE): nothing to pre-arm")
    lines.append(f"arrival predicted within (median): {r['median_eta_err_s']} s, over {r['predictions']} prediction(s) "
                 "with a direction (none: pre-armed by distance)")
    p = r["prior"]
    if p:
        lines.append(f"site's usual front after the run ({p.source}, {p.fronts} front(s)): toward {p.direction_deg:.0f}° "
                     f"at {p.speed_mps:.2f} m/s, agreement {p.agreement:.2f}"
                     + (" (used)" if p.slowness(0.7, 3) else " (not used: the fronts do not agree)"))
    return "\n".join(lines)