"""How good the arc predictor is (M5a), on runs it never saw.

1. Per window: ROC AUC, log loss, calibration (when it says 0.7, do arcs follow 70 % of the time?), and the
   share of arc windows warned at the safety core's threshold.
2. Closed loop: a node in fog whose controller keeps pushing the HV up (as M6 does, to collect more), with
   the safety core's rules: back-off (level - 0.12 for 4 s) after a warning or an arc, and a FAULT after 3 arcs
   within 60 s, which keeps the HV off until an operator arms the node again (taken as 5 minutes). Arcs
   happen at random from the true hazard. Compared: no predictor (only arcs make it back off), the rule-based fallback, the CNN.
   Reported: arcs, warnings and faults per hour, the average HV level, and the water collected relative to no
   predictor (capture 1 - exp(-6 (V - V0)), as in sim/fake_node.py): what the predictor costs in collection.
"""

import math

import numpy as np

from . import arc_cnn as cnn
from . import arc_physics as phys


def auc(prob, y):
    """Area under the ROC curve, by ranks (ties averaged)."""
    prob, y = np.asarray(prob, dtype=np.float64), np.asarray(y).astype(bool)
    order = np.argsort(prob, kind="mergesort")
    ranks = np.empty(len(prob))
    sorted_p = prob[order]
    i = 0
    while i < len(prob):
        j = i
        while j + 1 < len(prob) and sorted_p[j + 1] == sorted_p[i]:
            j += 1
        ranks[order[i:j + 1]] = (i + j) / 2 + 1
        i = j + 1
    pos, neg = y.sum(), (~y).sum()
    return float((ranks[y].sum() - pos * (pos + 1) / 2) / (pos * neg)) if pos and neg else float("nan")


def window_report(name, prob, y, threshold):
    prob, y = np.asarray(prob), np.asarray(y).astype(bool)
    warned = prob > threshold
    bins = []
    for lo, hi in zip(np.linspace(0, 1, 6)[:-1], np.linspace(0, 1, 6)[1:]):
        sel = (prob >= lo) & (prob < hi if hi < 1 else prob <= hi)
        if sel.any():
            bins.append((round(lo, 1), round(hi, 1), int(sel.sum()), float(prob[sel].mean()), float(y[sel].mean())))
    return {"name": name, "auc": auc(prob, y), "log_loss": cnn.log_loss(prob, y),
            "warned_arc_windows": float(warned[y].mean()) if y.any() else None,
            "warned_safe_windows": float(warned[~y].mean()) if (~y).any() else None, "calibration": bins}


def closed_loop(predict, threshold, runs=8, minutes=30, seed=100, dt=0.1, climb_per_s=0.005,
                backoff_s=4.0, backoff_drop=0.12, max_arcs=3, arc_window_s=60.0, rearm_s=300.0):
    """predict(window µA) -> risk, or None for no predictor. Returns the totals over all runs."""
    rng = np.random.default_rng(seed)
    arcs = warnings = faults = 0
    level_sum = capture_sum = steps = 0
    for _ in range(runs):
        p = phys.draw_params(rng)
        level, cap_until, cap = p.v0 + 0.15, -1.0, 1.0
        off_until, recent = -1.0, []
        phase = rng.uniform(0, 2 * math.pi)
        for k in range(int(minutes * 60 / dt)):
            t = k * dt
            fog = 0.7 + 0.3 * math.sin(2 * math.pi * t / 600 + phase)        # thicker and thinner fog, 10 min
            steps += 1
            if t < off_until:                                                # FAULT: HV off, nothing collected
                continue
            limit = cap if t < cap_until else 0.95
            level = min(level + climb_per_s * dt, limit, 0.95)
            level_sum += level
            capture_sum += fog * (1 - math.exp(-6 * max(level - p.v0, 0)))
            backoff = False
            if rng.random() < 1 - math.exp(-phys.hazard(p, level, fog) * dt):
                arcs += 1
                backoff = True
                recent = [a for a in recent if t - a <= arc_window_s] + [t]
                if len(recent) >= max_arcs:
                    faults += 1
                    recent = []
                    off_until, level, cap_until = t + rearm_s, p.v0 + 0.15, -1.0
                    continue
            elif predict is not None:
                risk = predict(phys.window(p, level, fog, rng))
                if risk > threshold:
                    warnings += 1
                    backoff = True
            if backoff:
                level = max(p.v0 + 0.1, level - backoff_drop)
                cap, cap_until = level, t + backoff_s
    hours = runs * minutes / 60
    return {"arcs_per_h": arcs / hours, "warnings_per_h": warnings / hours, "faults_per_h": faults / hours,
            "mean_level": level_sum / max(steps, 1), "capture": capture_sum / steps}


def predictor_from(q, rule, use_model=True):
    """A window -> risk function, as the firmware computes it (int8 CNN, or the rule)."""
    def predict(x):
        z, sigma, median = cnn.preprocess(x[None, :])
        if use_model:
            return float(q.predict(z)[0])
        return float(cnn.rule_predict(z, sigma, median, rule)[0])
    return predict