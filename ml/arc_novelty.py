"""Novelty guard for the arc predictor (M5a): "the AI that knows when it does not know".

The CNN was trained on synthetic windows. On the real HV stage it will meet windows unlike anything it saw
(another current scale, more noise, a loose wire...). Its risk is then a guess. The guard measures how far a
window is from the training windows and, when it is too far, the node:
  - uses the rule-based fallback instead of the CNN (as it already does for a window of the wrong length),
  - sends the window as a "wave" message with trigger NOVEL, so M11 can learn from it.

Three checks, each cheap, on numbers the firmware already computes for every window. Novel = any of them:
  1. saturation: 4 samples or more equal to the window's maximum. A 16-bit ADC reading a noisy current
     almost never repeats its top value; a clipped input (gain too high) does it all the time.
  2. physical range: the rule's 3 inputs, log(1 + pulses/s), log(noise), log(median µA), outside the range
     seen in training (widened by a margin). The CNN scales each window by its median, so its features
     cannot see a change of current scale or a dead sensor: these three can.
  3. shape: Mahalanobis distance of the CNN's 32 pooled features, log(1 + f) (they are >= 0 and
     heavy-tailed), d² = |W (g − µ)|², covariance shrunk (Ledoit-Wolf) so dead channels do not blow it up.
The range margin and the distance threshold are set on the VALIDATION runs so that, together, about
FALSE_NOVEL_TARGET of normal windows are called novel: chosen, not hoped for.

What the node does with a novel window (chosen from the evaluation below, see docs/edge_ai.md): it keeps the
CNN's risk (switching to the rule made things worse in every shifted condition tested), sends the window as a
wave with trigger NOVEL, and reports the share of novel windows, so the controller (M6) stops climbing while the
model is out of its depth.

python -m ml novelty  fits it on the current model's training data, evaluates it, and writes
firmware/node/lib/edge_ai/arc_novelty_data.h.
"""
import json
import os
from dataclasses import replace

import numpy as np

from . import arc_cnn as cnn
from . import arc_physics as phys

FALSE_NOVEL_TARGET = 0.01      # about 1 % of normal windows may be called novel (all checks together)
SATURATION_TIES = 4            # this many samples equal to the window's maximum: clipped input
DIMS = cnn.FEATURES
PHYS = 3


def features(q, X):
    """(N, WINDOW) µA -> dict: 'shape' (N, 32) log(1 + CNN pooled features), 'phys' (N, 3) the rule's inputs,
    'ties' (N,) samples equal to the window's maximum."""
    X = np.asarray(X, dtype=np.float32)
    z, sigma, median = cnn.preprocess(X)
    _, feat = q.accumulate(cnn.quantize_input(z))
    ties = (X == X.max(axis=1, keepdims=True)).sum(axis=1)
    return {"shape": np.log1p(feat.astype(np.float64)), "phys": cnn.rule_inputs(z, sigma, median),
            "ties": ties}


def ledoit_wolf(x):
    """Shrunk covariance of centred data x (n, p), Ledoit & Wolf (2004), as sklearn.covariance.LedoitWolf:
    (1 - a) S + a mu I, with S the sample covariance (1/n), mu = tr(S)/p and the shrinkage a estimated from
    the data. Numpy only, so the laptop needs nothing more than requirements-ml.txt."""
    n, p = x.shape
    s = x.T @ x / n
    mu = np.trace(s) / p
    delta = ((s - mu * np.eye(p)) ** 2).sum() / p
    x2 = x ** 2
    beta = min(((x2.T @ x2) / n - s ** 2).sum() / (p * n), delta)
    a = beta / delta if delta > 0 else 0.0
    return (1 - a) * s + a * mu * np.eye(p)


class NoveltyGuard:
    def __init__(self, mean, whiten, threshold, phys_lo, phys_hi):
        self.mean = np.asarray(mean, dtype=np.float64)
        self.whiten = np.asarray(whiten, dtype=np.float64)
        self.threshold = float(threshold)
        self.phys_lo = np.asarray(phys_lo, dtype=np.float64)
        self.phys_hi = np.asarray(phys_hi, dtype=np.float64)

    @classmethod
    def fit(cls, f_train, f_val, false_novel=FALSE_NOVEL_TARGET):
        g = f_train["shape"]
        mean = g.mean(axis=0)
        cov = ledoit_wolf(g - mean)
        whiten = np.linalg.cholesky(np.linalg.inv(cov)).T   # d² = |Lᵀ (g-µ)|², Σ⁻¹ = L Lᵀ
        lo, hi = f_train["phys"].min(axis=0), f_train["phys"].max(axis=0)
        span = hi - lo
        guard = cls(mean, whiten, np.inf, lo - 0.1 * span, hi + 0.1 * span)
        # half of the budget to the range check (widen until it is met), the rest to the distance
        for margin in np.arange(0.1, 2.01, 0.05):
            guard.phys_lo, guard.phys_hi = lo - margin * span, hi + margin * span
            if guard.out_of_range(f_val).mean() <= false_novel / 2:
                break
        others = guard.saturated(f_val) | guard.out_of_range(f_val)
        rest = max(false_novel - others.mean(), false_novel / 4)
        guard.threshold = float(np.quantile(guard.distance2(f_val)[~others], 1.0 - rest))
        return guard

    def distance2(self, f):
        d = (np.asarray(f["shape"], dtype=np.float64) - self.mean) @ self.whiten.T
        return (d * d).sum(axis=1)

    @staticmethod
    def saturated(f):
        return f["ties"] >= SATURATION_TIES

    def out_of_range(self, f):
        p = f["phys"]
        return ((p < self.phys_lo) | (p > self.phys_hi)).any(axis=1)

    def why(self, f):
        """Per window: 0 known, 1 saturated, 2 out of physical range, 3 unusual shape (first that applies)."""
        sat, rng_, shp = self.saturated(f), self.out_of_range(f), self.distance2(f) > self.threshold
        return np.where(sat, 1, np.where(rng_, 2, np.where(shp, 3, 0)))

    def novel(self, f):
        return self.why(f) > 0


# ---------------------------------------------------------------- shifted conditions (out of distribution)

def shifted_dataset(n_runs, windows_per_run, seed, shift):
    """Like phys.make_dataset, but every run's parameters are changed by `shift` (a dict of factors), or the
    windows are post-processed (offset, clipping, disconnected sensor). Labels come from the same physics."""
    rng = np.random.default_rng(seed)
    n = n_runs * windows_per_run
    X = np.empty((n, phys.WINDOW), dtype=np.float32)
    y = np.empty(n, dtype=np.int8)
    i = 0
    for _ in range(n_runs):
        p = phys.draw_params(rng)
        p = replace(p, **{k: getattr(p, k) * f for k, f in shift.get("scale", {}).items()})
        for _ in range(windows_per_run):
            v, fog = phys.draw_condition(p, rng)
            x = phys.window(p, v, fog, rng)
            if "offset_ua" in shift:
                x = np.maximum(x + shift["offset_ua"], 0.0)
            if "clip_ua" in shift:
                x = np.minimum(x, shift["clip_ua"])
            if shift.get("disconnected"):
                x = np.abs(rng.normal(0.0, 0.3, phys.WINDOW))
            X[i] = x
            y[i] = rng.random() < phys.arc_probability(p, v, fog)
            i += 1
    return X, y


SHIFTS = {
    # what could really differ on the bench, and why
    "current x0.1 (weak ion module)": {"scale": {"k": 0.1}},
    "noise x3 (long unshielded wires)": {"scale": {"noise_rel": 3.0, "noise_abs": 3.0}},
    "mains ripple x5 (cheap 12 V adapter)": {"scale": {"ripple_rel": 5.0}},
    "interference x10 (PWM switching nearby)": {"scale": {"emi_rate": 10.0}},
    "pulses 3x smaller (other electrode)": {"scale": {"pulse_amp": 1 / 3}},
    "ADC clipping at 60 uA (gain too high)": {"clip_ua": 60.0},
    "+15 uA offset (zero not calibrated)": {"offset_ua": 15.0},
    "sensor disconnected": {"disconnected": True},
}


# ---------------------------------------------------------------- C header

def header(guard, model_name, notes):
    def arr(name, values, per_line=8):
        def lit(v):  # a valid C float literal: 0 -> 0.0f, 1e-05 -> 1e-05f, 2 -> 2.0f
            t = f"{float(v):.9g}"
            return (t if any(c in t for c in ".einf") else t + ".0") + "f"
        vals = [lit(v) for v in np.ravel(values)]
        rows = [", ".join(vals[i:i + per_line]) for i in range(0, len(vals), per_line)]
        return f"static const float {name}[{len(vals)}] = {{\n    " + ",\n    ".join(rows) + "};\n"
    return ("// Novelty guard of the arc predictor (M5a). GENERATED by python -m ml novelty: do not edit.\n"
            f"// For model {model_name}. {notes}\n"
            "#pragma once\n\n"
            f'#define ARC_NOVELTY_MODEL "{model_name}"\n'
            f"#define ARC_NOVELTY_DIMS {DIMS}\n"
            f"#define ARC_NOVELTY_SATURATION_TIES {SATURATION_TIES}\n"
            f"#define ARC_NOVELTY_THRESHOLD {guard.threshold:.9g}f   // squared Mahalanobis distance\n\n"
            + "// Physical range of log(1 + pulses/s), log(noise + 1e-3), log(max(median µA, 0.1))\n"
            + arr("kNoveltyPhysLo", guard.phys_lo) + arr("kNoveltyPhysHi", guard.phys_hi) + "\n"
            + "// Shape: g = log(1 + pooled feature); d² = |W (g - mean)|², W upper-triangular, row-major\n"
            + arr("kNoveltyMean", guard.mean) + "\n"
            + arr("kNoveltyWhiten", guard.whiten))


def evaluate(q, rule, guard, runs=20, windows=200, seed=7000):
    """For each condition: share of windows called novel, and the risk quality (AUC, log loss) of the CNN,
    the rule, and the guarded predictor (CNN, or the rule on novel windows)."""
    from .arc_eval import auc
    rows = []
    sets = {"in distribution (unseen runs)": None, **SHIFTS}
    for k, (name, shift) in enumerate(sets.items()):
        if shift is None:
            X, y, _ = phys.make_dataset(runs, windows, seed)
        else:
            X, y = shifted_dataset(runs, windows, seed + 100 + k, shift)
        z, sigma, median = cnn.preprocess(X)
        p_cnn = q.predict(z)
        p_rule = cnn.rule_predict(z, sigma, median, rule)
        why = guard.why(features(q, X))
        novel = why > 0
        p_switch = np.where(novel, p_rule, p_cnn)
        p_max = np.where(novel, np.maximum(p_rule, p_cnn), p_cnn)
        row = {"condition": name, "windows": int(len(y)), "arc_share": float(y.mean()),
               "novel": float(novel.mean()), "saturated": float((why == 1).mean()),
               "out_of_range": float((why == 2).mean()), "shape": float((why == 3).mean())}
        for tag, p in (("cnn", p_cnn), ("rule", p_rule), ("switch", p_switch), ("max", p_max)):
            row[f"auc_{tag}"] = float(auc(p, y)) if 0 < y.sum() < len(y) else None
            row[f"logloss_{tag}"] = float(cnn.log_loss(p, y))
            row[f"warned_safe_{tag}"] = float((p[y == 0] > 0.3).mean()) if (y == 0).any() else None
            row[f"warned_arc_{tag}"] = float((p[y == 1] > 0.3).mean()) if (y == 1).any() else None
        rows.append(row)
    return rows


# ---------------------------------------------------------------- one guard per model version

def sidecar(model_path):
    """ml/models/<name>.novelty.json, next to the model's .npz."""
    model_path = str(model_path)
    return model_path[:-4] + ".novelty.json" if model_path.endswith(".npz") else model_path + ".novelty.json"


def fit_for(q, X_train, X_val):
    """The guard of a model, from the windows it was trained on and windows it was not."""
    return NoveltyGuard.fit(features(q, X_train), features(q, X_val))


def save(guard, path, info=None):
    with open(path, "w") as f:
        json.dump({"mean": guard.mean.tolist(), "whiten": guard.whiten.tolist(), "threshold": guard.threshold,
                   "phys_lo": guard.phys_lo.tolist(), "phys_hi": guard.phys_hi.tolist(), **(info or {})}, f)


def load(path):
    with open(path) as f:
        d = json.load(f)
    return NoveltyGuard(d["mean"], d["whiten"], d["threshold"], d["phys_lo"], d["phys_hi"]), d


def write_header(guard, model_name, notes, out_header):
    with open(out_header, "w", newline="\n") as f:
        f.write(header(guard, model_name, notes))


def for_saved_model(model_path, log=print):
    """The guard of a saved model: its sidecar file, or, for a model trained before the guard existed on
    synthetic data only, fitted again on the same synthetic runs (same seeds as arc-train)."""
    q, rule, info = cnn.load_model(model_path)
    path = sidecar(model_path)
    if os.path.exists(path):
        guard, meta = load(path)
        return q, rule, info, guard, meta.get("notes", "")
    if info.get("data") != "synthetic":
        raise FileNotFoundError(f"{path}: no novelty guard saved with this model, and its training windows are "
                                "recorded runs that cannot be rebuilt here. Run the pipeline again.")
    s = info["seed"] * 1000
    Xtr, _, _ = phys.make_dataset(info["train_runs"], info["windows_per_run"], s + 1)  # the CNN's own data
    Xva, _, _ = phys.make_dataset(max(10, info["train_runs"] // 4), info["windows_per_run"], s + 2)
    guard = fit_for(q, Xtr, Xva)
    notes = (f"Fitted on its {len(Xtr)} training windows; thresholds set on {len(Xva)} validation windows for "
             f"{100 * FALSE_NOVEL_TARGET:.0f} % of normal windows called novel.")
    save(guard, path, {"model": info["name"], "notes": notes})
    log(f"novelty guard fitted for {info['name']}: {path}")
    return q, rule, info, guard, notes


def run(model_path, out_header, report_path=None, log=print):
    """python -m ml novelty: the guard of a saved model, its evaluation, and the firmware header."""
    q, rule, info, guard, notes = for_saved_model(model_path, log)
    rows = evaluate(q, rule, guard)
    write_header(guard, info["name"], notes, out_header)
    if report_path:
        with open(report_path, "w") as f:
            json.dump({"model": info["name"], "threshold": guard.threshold, "rows": rows}, f, indent=1)
    return guard, rows
