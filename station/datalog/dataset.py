"""Turn one run into labelled tables (CSV) and a summary (JSON), in <run>/dataset/:

    telemetry.csv   one row per node per second: every contract field + the labels (labels.py)
    events.csv      every event, with its data as compact JSON
    windows.csv     current windows (wave messages) labelled arc / pre_arc / normal, for the arc predictor
    segments.csv    one row per fog episode per node: water collected, energy, g/Wh, arcs per minute
    summary.json    what went in, what was rejected, and the totals per node

Column names come from the contract schema, so a field added to the contract appears here by itself.
"""

import bisect
import csv
import json
from datetime import datetime
from pathlib import Path

import aeroion_contract as ac

from . import labels, reader, runs

WINDOW_LABELS = {"ARC": "arc", "ARC_PREDICTED": "pre_arc", "MANUAL": "normal"}  # other triggers: lower case
EVENT_COLUMNS = ["run_id", "node_id", "ts", "seq", "type", "data"]
WINDOW_COLUMNS = ["run_id", "node_id", "ts", "seq", "trigger", "label", "arc_followed", "intervened", "fs_hz",
                  "n_samples", "samples"]
SEGMENT_COLUMNS = ["run_id", "node_id", "segment", "start_ts", "end_ts", "duration_s", "completed",
                   "mass_gain_g", "rate_gpm", "energy_wh", "g_per_wh", "hv_on_fraction", "mean_hv_level",
                   "mean_fog_idx", "max_fog_idx", "arc_warnings", "arcs", "arcs_per_min"]


def telemetry_columns(schema=None):
    """Telemetry fields in the contract's order; nested objects are flattened (health.score -> health_score)."""
    schema = schema or ac.SCHEMAS["telemetry"]
    columns = []
    for name, prop in schema["properties"].items():
        if prop.get("type") == "object" and prop.get("properties"):
            columns += [f"{name}_{sub}" for sub in prop["properties"]]
        else:
            columns.append(name)
    return columns


def flatten(payload):
    row = {}
    for key, value in payload.items():
        if isinstance(value, dict):
            for sub, sub_value in value.items():
                row[f"{key}_{sub}"] = sub_value
        else:
            row[key] = value
    return row


def build(run_dir, config_path=None):
    """Build every table of one run. Returns the summary (also written to summary.json)."""
    run_dir = Path(run_dir)
    meta = runs.read_meta(run_dir)
    config = labels.load_config(config_path or run_dir / runs.CONFIG_FILE)
    messages = reader.read_run(run_dir / runs.RAW_FILE)
    run_id = meta["run_id"]

    tables = {"telemetry": [], "events": [], "windows": [], "segments": []}
    per_node = {}
    for node in messages.nodes():
        rows = [flatten(m) for m in messages.telemetry[node]]
        events = messages.events[node]
        arc_times = sorted(e["ts"] for e in events if e["type"] == "ARC")
        labels.apply_labels(rows, arc_times, config)
        for row in rows:
            row["run_id"] = run_id
        tables["telemetry"] += rows
        tables["events"] += [{"run_id": run_id, "node_id": node, "ts": e["ts"], "seq": e["seq"], "type": e["type"],
                              "data": json.dumps(e["data"], separators=(",", ":"))} for e in events]
        predicted = sorted(e["ts"] for e in events if e["type"] == "ARC_PREDICTED")
        tables["windows"] += window_rows(run_id, node, messages.waves[node], arc_times, config, predicted)
        tables["segments"] += segment_rows(run_id, node, rows, events)
        per_node[node] = node_summary(rows, events, messages.quality[node])

    out_dir = run_dir / runs.DATASET_DIR
    out_dir.mkdir(exist_ok=True)
    write_csv(out_dir / "telemetry.csv", ["run_id"] + telemetry_columns() + labels.label_columns(),
              tables["telemetry"])
    write_csv(out_dir / "events.csv", EVENT_COLUMNS, tables["events"])
    write_csv(out_dir / "windows.csv", WINDOW_COLUMNS, tables["windows"])
    write_csv(out_dir / "segments.csv", SEGMENT_COLUMNS, tables["segments"])

    summary = {
        "run_id": run_id,
        "built": datetime.now().isoformat(timespec="seconds"),
        "contract_version": ac.SCHEMA_VERSION,
        "settings": meta.get("settings", {}),
        "label_config": config,
        "input": messages.stats,
        "problems": messages.problems,
        "rows": {name: len(rows) for name, rows in tables.items()},
        "nodes": per_node,
    }
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def write_csv(path, columns, rows):
    """None becomes an empty cell, True/False become 1/0, so pandas and spreadsheets read them easily."""
    with open(path, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(columns)
        for row in rows:
            writer.writerow([_cell(row.get(column)) for column in columns])


def _cell(value):
    if value is None:
        return ""
    if isinstance(value, bool):
        return int(value)
    return value


def window_rows(run_id, node, waves, arc_times, config, predicted_times=()):
    """intervened = 1 when the safety core backed off on a prediction (ARC_PREDICTED) within the horizon after
    the window. Then "no arc followed" proves nothing: the back-off may have prevented it. Training on such
    windows teaches the model that its warnings were wrong (intervention bias): M11 leaves them out."""
    horizon_ms = config["arc_horizon_s"] * 1000
    predicted_times = list(predicted_times)
    rows = []
    for w in waves:
        i = bisect.bisect_left(arc_times, w["ts"])
        followed = i < len(arc_times) and arc_times[i] <= w["ts"] + horizon_ms
        k = bisect.bisect_left(predicted_times, w["ts"])
        intervened = w["trigger"] == "ARC_PREDICTED" or (
            k < len(predicted_times) and predicted_times[k] <= w["ts"] + horizon_ms)
        label = WINDOW_LABELS.get(w["trigger"], w["trigger"].lower())
        rows.append({"run_id": run_id, "node_id": node, "ts": w["ts"], "seq": w["seq"], "trigger": w["trigger"],
                     "label": label, "arc_followed": int(followed), "intervened": int(intervened),
                     "fs_hz": w["fs_hz"],
                     "n_samples": len(w["samples"]), "samples": " ".join(f"{s:g}" for s in w["samples"])})
    return rows


def segment_rows(run_id, node, rows, events):
    """One row per fog episode: from FOG_DETECTED to FOG_CLEARED (or to the end of the run)."""
    segments, start = [], None
    for e in events:
        if e["type"] == "FOG_DETECTED" and start is None:
            start = e["ts"]
        elif e["type"] == "FOG_CLEARED" and start is not None:
            segments.append(_segment(run_id, node, len(segments) + 1, start, e["ts"], True, rows, events))
            start = None
    if start is not None and rows:
        segments.append(_segment(run_id, node, len(segments) + 1, start, rows[-1]["ts"], False, rows, events))
    return segments


def _segment(run_id, node, index, start, end, completed, rows, events):
    inside = [r for r in rows if start <= r["ts"] <= end]
    minutes = (end - start) / 60000.0
    gain = _change(inside, "mass_g")
    energy = _change(inside, "e_wh")
    hv_levels = [r["hv_level"] for r in inside if r.get("hv_on")]
    fog = [r["fog_idx_opt"] for r in inside if r.get("fog_idx_opt") is not None]
    arcs = sum(1 for e in events if e["type"] == "ARC" and start <= e["ts"] <= end)
    warnings = sum(1 for e in events if e["type"] == "ARC_PREDICTED" and start <= e["ts"] <= end)
    return {
        "run_id": run_id, "node_id": node, "segment": index, "start_ts": start, "end_ts": end,
        "duration_s": round(minutes * 60, 1), "completed": completed,
        "mass_gain_g": _round(gain),
        "rate_gpm": _round(gain / minutes if gain is not None and minutes > 0 else None),
        "energy_wh": _round(energy),
        "g_per_wh": _round(gain / energy if gain is not None and energy else None),
        "hv_on_fraction": _round(len(hv_levels) / len(inside) if inside else None),
        "mean_hv_level": _round(sum(hv_levels) / len(hv_levels) if hv_levels else None),
        "mean_fog_idx": _round(sum(fog) / len(fog) if fog else None),
        "max_fog_idx": _round(max(fog) if fog else None),
        "arc_warnings": warnings, "arcs": arcs,
        "arcs_per_min": _round(arcs / minutes if minutes > 0 else None),
    }


def node_summary(rows, events, quality):
    arcs = [e for e in events if e["type"] == "ARC"]
    caught = sum(1 for e in arcs if e["data"]["predicted"])
    return {
        "telemetry_rows": len(rows),
        "duration_s": round((rows[-1]["ts"] - rows[0]["ts"]) / 1000, 1) if len(rows) > 1 else 0,
        "mass_change_g": _round(_change(rows, "mass_g")),
        "energy_wh": _round(_change(rows, "e_wh")),
        "fog_episodes": sum(1 for e in events if e["type"] == "FOG_DETECTED"),
        "arc_warnings": sum(1 for e in events if e["type"] == "ARC_PREDICTED"),
        "arcs": len(arcs),
        "arcs_predicted_pct": _round(100.0 * caught / len(arcs) if arcs else None),
        "lost_messages": quality["lost"],
        "restarts": quality["restarts"],
    }


def _change(rows, field):
    values = [r[field] for r in rows if r.get(field) is not None]
    return values[-1] - values[0] if len(values) >= 2 else None


def _round(value, digits=4):
    return None if value is None else round(value, digits)