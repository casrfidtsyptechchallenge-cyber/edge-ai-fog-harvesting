"""Labels added to every telemetry row.

Each label is one small function registered with @labeler("column", ...). To add a label, write a new
function here: its columns appear in telemetry.csv automatically. To remove one, delete its function.
Thresholds live in DEFAULT_CONFIG; a run can override them with its own label_config.json.
"""

import bisect
import json
from pathlib import Path

DEFAULT_CONFIG = {
    "fog_thresholds": [0.15, 0.45],  # fog_idx_opt below the 1st = "none", below the 2nd = "light", else "dense"
    "rate_window_s": 30,  # centred window used for the collection-rate label
    "rate_min_samples": 10,  # fewer mass readings than this in the window: no rate label
    "mass_reset_drop_g": 2.0,  # a sudden drop bigger than this is a TARE or a restart, not water loss
    "arc_horizon_s": 5,  # label_arc_next = 1 if an arc happens within this many seconds
    "exclude_states": ["SAFE", "MAINTENANCE"],  # rows in these states get an exclude_reason
    "exclude_person_near": True,  # rows where the radar sees someone get an exclude_reason
}


def load_config(path=None):
    """Default settings, updated with the keys found in `path` (if it exists). Unknown keys are errors,
    so a typo cannot silently fall back to a default."""
    config = dict(DEFAULT_CONFIG)
    if path and Path(path).exists():
        override = json.loads(Path(path).read_text(encoding="utf-8"))
        unknown = sorted(set(override) - set(DEFAULT_CONFIG))
        if unknown:
            raise ValueError(f"unknown label setting(s) {unknown} in {path}")
        config.update(override)
    return config


class Labeler:
    def __init__(self, name, columns, func):
        self.name = name
        self.columns = columns
        self.func = func


LABELERS = []


def labeler(*columns):
    """Register a labeling function that fills the given columns of each row."""

    def register(func):
        LABELERS.append(Labeler(func.__name__, columns, func))
        return func

    return register


def label_columns():
    return [column for lb in LABELERS for column in lb.columns]


def apply_labels(rows, arc_times_ms, config):
    """Label one node's rows in place. `rows` are flattened telemetry sorted by ts; `arc_times_ms` are the
    sorted timestamps of that node's ARC events."""
    context = {"config": config, "arc_times_ms": arc_times_ms}
    for lb in LABELERS:
        lb.func(rows, context)


# ------------------------------------------------------------------------------------------ the labels


@labeler("label_fog_class")
def fog_class(rows, context):
    """none / light / dense from the optical fog index. Teacher label for the virtual fog sensor (M5b)."""
    low, high = context["config"]["fog_thresholds"]
    for row in rows:
        fog = row.get("fog_idx_opt")
        if fog is None:
            row["label_fog_class"] = None
        else:
            row["label_fog_class"] = "none" if fog < low else ("light" if fog < high else "dense")


@labeler("label_rate_gpm")
def collection_rate(rows, context):
    """Collection rate (g/min) from the slope of mass_g over a window centred on each row. Better than the
    node's own rate_gpm because it also uses later readings, which only exist offline. Target for the
    yield model (M6)."""
    config = context["config"]
    half_ms = config["rate_window_s"] * 1000 / 2
    for row in rows:
        row["label_rate_gpm"] = None
    for epoch in _mass_epochs(rows, config["mass_reset_drop_g"]):
        times = [row["ts"] for row in epoch]
        for row in epoch:
            lo = bisect.bisect_left(times, row["ts"] - half_ms)
            hi = bisect.bisect_right(times, row["ts"] + half_ms)
            window = epoch[lo:hi]
            if len(window) >= config["rate_min_samples"]:
                slope = _slope_per_minute([(r["ts"], r["mass_g"]) for r in window])
                row["label_rate_gpm"] = None if slope is None else round(slope, 4)


@labeler("label_arc_next")
def arc_ahead(rows, context):
    """1 if an arc happens within the next arc_horizon_s seconds, else 0. Target for arc prediction (M5a)."""
    horizon_ms = context["config"]["arc_horizon_s"] * 1000
    arcs = context["arc_times_ms"]
    for row in rows:
        i = bisect.bisect_right(arcs, row["ts"])
        row["label_arc_next"] = int(i < len(arcs) and arcs[i] <= row["ts"] + horizon_ms)


@labeler("exclude_reason")
def exclusions(rows, context):
    """Why a row should not be used for training ("" = usable). Rows are kept; the trainer filters them."""
    config = context["config"]
    for row in rows:
        reasons = []
        if row.get("state") in config["exclude_states"]:
            reasons.append("state")
        if config["exclude_person_near"] and row.get("radar_presence") is True:
            reasons.append("person_near")
        row["exclude_reason"] = ";".join(reasons)


# ------------------------------------------------------------------------------------------ helpers


def _mass_epochs(rows, drop_g):
    """Split the rows that have a mass into stretches without a reset (TARE or node restart)."""
    epochs, current, previous = [], [], None
    for row in rows:
        mass = row.get("mass_g")
        if mass is None:
            continue
        if previous is not None and previous - mass > drop_g:
            epochs.append(current)
            current = []
        current.append(row)
        previous = mass
    if current:
        epochs.append(current)
    return epochs


def _slope_per_minute(points):
    """Least-squares slope of (time ms, value) points, in value units per minute."""
    n = len(points)
    t0 = points[0][0]
    ts = [(t - t0) / 60000.0 for t, _ in points]
    vs = [v for _, v in points]
    mean_t = sum(ts) / n
    mean_v = sum(vs) / n
    sxx = sum((t - mean_t) ** 2 for t in ts)
    if sxx == 0:
        return None
    return sum((t - mean_t) * (v - mean_v) for t, v in zip(ts, vs)) / sxx