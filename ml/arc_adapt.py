"""On-node learning for the arc predictor (M5a): auto-labels, last-layer adaptation, federated averaging.

Why: the CNN was trained on synthetic windows. Each real node has its own electrode, wiring and noise. The
novelty guard (arc_novelty.py) flags gross differences; subtle ones (smaller pre-discharge pulses...) it cannot
see. A node can learn them from its own arcs.

1. Auto-labels. The node logs one window's 32 pooled CNN features a second (33 bytes). An arc measured by the
   hardware (current above 250 µA) is certain: the windows of the 5 s before it are labelled 1. A window is
   labelled 0 only if nothing happened in the next 5 s: no arc AND no back-off. A back-off may have prevented
   an arc, so a "no arc" after it proves nothing: those windows are dropped (censored). Labelling them 0 would
   teach the model that its warnings were wrong (intervention bias; the ablation below measures it).
2. Last-layer adaptation. Only the dense layer changes (32 weights + bias = 33 numbers); the convolutions stay.
   Logistic regression on the node's labelled features, by Newton's method, with a ridge pull towards the
   factory weights (on standardised features), so that a node with few arcs keeps most of what it knew.
3. Gate. The newest 25 % of the labelled windows are held out. New weights are adopted only if they lower the
   log loss there; else the node keeps what it has. M3 keeps the last word whatever the model says.
4. Federated averaging (FedAvg). The station averages the nodes' 33 numbers, weighted by their labelled windows,
   and sends the mean back; each node gates it on its own held-out windows. No window leaves a node.

python -m ml adapt  runs the whole thing on simulated nodes and reports it (results/arc_adapt.json).
"""
import math
from dataclasses import replace

import numpy as np

from . import arc_cnn as cnn
from . import arc_physics as phys

HORIZON_S = phys.HORIZON_S
LOG_EVERY_S = 1.0          # one window's features logged per second
BUFFER = 1200              # the node keeps its last 1200 probe windows (32 feature bytes + label, 40 kB),
                           # from round to round: about 4 h of probes
HOLDOUT = 0.25             # newest share of the labelled windows kept for the gate
RIDGE = 30.0               # pull towards the factory weights (standardised features): see tune_ridge()
NEWTON_STEPS = 8
MIN_POS_TRAIN, MIN_POS_HOLD = 5, 2
BIAS_PULL = 1.0            # the bias's share of the ridge (features are standardised: comparable)
MAX_UP, MAX_DOWN = 2.0, 0.5   # trust region, asymmetric the safe way: in one round the mean logit of the node's
                               # windows may rise by 2 (more careful) but fall by 0.5 only (less careful)
# Labelling probes. Censoring drops every window followed by a back-off; a node whose model warns all the time
# (false alarms on a shifted node) then never gets a label and can never learn that its alarms were false.
# So every PROBE_EVERY_S, for PROBE_S, the node does not act on the CNN's warnings and keeps climbing as usual
# (a short controlled sweep); the safety core's own arc detection (250 µA, back-off, fault after 3 arcs) stays
# on. Windows of a probe had no intervention: their labels are unbiased, including on the risky side.
PROBE_EVERY_S, PROBE_S = 600.0, 60.0


# ---------------------------------------------------------------- the last layer as floats

class LastLayer:
    """risk = sigmoid(f · w + b), f the 32 pooled int8 features. Starts as the int8 model's dense layer."""

    def __init__(self, w, b):
        self.w = np.asarray(w, dtype=np.float64)
        self.b = float(b)

    @classmethod
    def factory(cls, q):
        return cls(q.w3 * q.logit_scale, q.b3 * q.logit_scale)

    def theta(self):
        return np.concatenate([self.w, [self.b]])

    @classmethod
    def from_theta(cls, t):
        return cls(t[:-1], t[-1])

    def risk(self, f):
        z = np.clip(np.asarray(f, dtype=np.float64) @ self.w + self.b, -30, 30)
        return 1 / (1 + np.exp(-z))


def feature_scale(q, X):
    """Standard deviation of each pooled feature on the training windows (1 where constant)."""
    _, f = q.accumulate(cnn.quantize_input(cnn.preprocess(X)[0]))
    s = f.std(axis=0)
    return np.where(s > 1e-6, s, 1.0)


def fit(F, y, start, scale, ridge=None, steps=NEWTON_STEPS):
    """Damped Newton's method on mean log loss + ridge/2n · |S (θ − θ0)|² (S = feature scales, BIAS_PULL for the
    bias), with backtracking: a step is halved until the objective goes down, so it cannot run off.
    Deterministic: the same on the node as here."""
    ridge = RIDGE if ridge is None else ridge
    F = np.asarray(F, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    n = len(y)
    A = np.hstack([F, np.ones((n, 1))])
    t0 = start.theta()
    s2 = np.concatenate([scale ** 2, [BIAS_PULL]])

    def objective(t):
        z = A @ t
        return float(np.mean(np.logaddexp(0, z) - y * z) + ridge / (2 * n) * np.sum(s2 * (t - t0) ** 2))

    t = t0.copy()
    f = objective(t)
    for _ in range(steps):
        p = 1 / (1 + np.exp(-np.clip(A @ t, -30, 30)))
        g = A.T @ (p - y) / n + ridge / n * s2 * (t - t0)
        H = (A * (p * (1 - p))[:, None]).T @ A / n + np.diag(ridge / n * s2 + 1e-9)
        d = np.linalg.solve(H, g)
        a = 1.0
        while a > 1e-4:
            fn = objective(t - a * d)
            if fn < f:
                break
            a *= 0.5
        if a <= 1e-4:
            break
        t, f = t - a * d, fn
    return LastLayer.from_theta(t)


def log_loss(p, y):
    p = np.clip(p, 1e-6, 1 - 1e-6)
    return float(-np.mean(y * np.log(p) + (1 - y) * np.log(1 - p)))


# ---------------------------------------------------------------- auto-labels

def auto_label(log, censor=True, unbiased=True):
    """log: dict with t (s), features (n, 32), arcs (times of measured arcs), actions (times of back-offs and
    faults), probe (window logged during a labelling probe). Returns features, labels, times, and whether each
    labelled window comes from a probe.
    unbiased (the default): only the windows of a probe are kept, with whatever followed them. Censoring alone
    is not enough: the windows it keeps are the ones the model found safe, so a model trained on them learns
    that everything is safe. Keeping the windows before every arc as well is not right either: arcs are then
    over-represented (38 % of the labels instead of ~8 %) and the model warns 3 to 5 times too often. Both
    biases were measured (python -m ml adapt). Probe windows had no intervention: an unbiased sample."""
    t = np.asarray(log["t"])
    probe = np.asarray(log.get("probe", np.zeros(len(t), bool)), dtype=bool)
    arcs = np.sort(np.asarray(log["arcs"]))
    acts = np.sort(np.asarray(log["actions"]))
    end = log["end"]
    keep, y = [], []
    for i, ti in enumerate(t):
        if ti + HORIZON_S > end:
            break                                    # its future is not known yet
        if unbiased and not probe[i]:
            continue                                 # outside a probe the model's own choices shaped what followed
        a = np.searchsorted(arcs, ti, side="right")
        if a < len(arcs) and arcs[a] <= ti + HORIZON_S:
            keep.append(i)
            y.append(1)
            continue
        if censor:
            k = np.searchsorted(acts, ti, side="right")
            if k < len(acts) and acts[k] <= ti + HORIZON_S:
                continue                             # a back-off came first: unknown what would have happened
        keep.append(i)
        y.append(0)
    keep = np.asarray(keep, dtype=int)
    return log["features"][keep], np.asarray(y, dtype=np.int8), t[keep], probe[keep]


def _losses(layer, F, y):
    p = np.clip(layer.risk(F), 1e-6, 1 - 1e-6)
    return -(y * np.log(p) + (1 - y) * np.log(1 - p))


def gate(current, candidate, F, y, z=2.0):
    """Adopt the candidate only if it lowers the log loss on the held-out windows by more than z standard errors
    of the paired difference (window by window): a better score by chance on a few arcs is not enough."""
    if len(y) < 20:
        return False
    d = _losses(candidate, F, y) - _losses(current, F, y)
    se = d.std(ddof=1) / math.sqrt(len(d))
    return d.mean() < 0 and -d.mean() > z * se


def split(F, y):
    n = len(y)
    cut = int(round(n * (1 - HOLDOUT)))
    return (F[:cut], y[:cut]), (F[cut:], y[cut:])


MIN_LABELS = 100            # labelled windows needed to try (arcs or not: false alarms teach too)


FOLDS = 4


def adapt_local(start, F, y, scale, probe=None):
    """One round on the node. The candidate is judged by blocked cross-validation in time: the store is cut in
    FOLDS consecutive blocks; each block's PROBE windows (the unbiased ones) are scored by a layer fitted on the
    other blocks. All probe windows judge, not a quarter of them: about four times the evidence. Adopted only if
    the paired log-loss gain over all of them is above 2 standard errors. Returns (layer, adopted?, reason)."""
    probe = np.ones(len(y), bool) if probe is None else np.asarray(probe, bool)
    n = len(y)
    if n < MIN_LABELS or (y[probe] == 1).sum() < MIN_POS_HOLD * FOLDS or probe.sum() < 20 * FOLDS:
        return start, False, "too few labelled windows (or arcs in probes) yet"
    edges = np.linspace(0, n, FOLDS + 1).astype(int)
    d = []
    for k in range(FOLDS):
        test = np.zeros(n, bool)
        test[edges[k]:edges[k + 1]] = True
        cand = shrink_to_trust(start, fit(F[~test], y[~test], start, scale), F[~test])
        sel = test & probe
        d.append(_losses(cand, F[sel], y[sel]) - _losses(start, F[sel], y[sel]))
    d = np.concatenate(d)
    se = d.std(ddof=1) / math.sqrt(len(d))
    if d.mean() < 0 and -d.mean() > 2.0 * se:
        return shrink_to_trust(start, fit(F, y, start, scale), F), True, "adopted"
    return start, False, "not clearly better in cross-validation"


def within_trust(old, new, F):
    lo = lambda layer: np.asarray(F, np.float64) @ layer.w + layer.b
    shift = float(np.mean(lo(new) - lo(old)))
    return -MAX_DOWN - 1e-9 <= shift <= MAX_UP + 1e-9


def shrink_to_trust(old, new, F):
    """The candidate moved back towards the old layer until the mean logit shift is within the trust region."""
    lo = lambda layer: np.asarray(F, np.float64) @ layer.w + layer.b
    shift = float(np.mean(lo(new) - lo(old)))
    a = 1.0
    if shift > MAX_UP:
        a = MAX_UP / shift
    elif shift < -MAX_DOWN:
        a = MAX_DOWN / -shift
    return LastLayer.from_theta(old.theta() + a * (new.theta() - old.theta()))


def fedavg(layers, counts):
    w = np.asarray(counts, dtype=np.float64)
    t = sum(c * l.theta() for c, l in zip(w, layers)) / w.sum()
    return LastLayer.from_theta(t)


# ---------------------------------------------------------------- simulated nodes (closed loop)

NODES = {   # one node per kind of difference from the training data
    "node A: pulses 3x smaller": {"pulse_amp": 1 / 3},
    "node B: current x0.3, noise x2": {"k": 0.3, "noise_rel": 2.0, "noise_abs": 2.0},
    "node C: like the training data": {},
}


def node_params(seed, shift):
    p = phys.draw_params(np.random.default_rng(seed))
    return replace(p, **{k: getattr(p, k) * f for k, f in shift.items()})


def closed_loop(p, q, layer, minutes, seed, threshold=0.3, dt=0.1, climb_per_s=0.005, backoff_s=4.0,
                backoff_drop=0.12, max_arcs=3, arc_window_s=60.0, rearm_s=300.0, probe=False,
                probe_every_s=PROBE_EVERY_S, probe_s=PROBE_S):
    """As arc_eval.closed_loop, for one node, with a last layer, logging what the node would log. Back-offs
    follow the safety core: a warning starts a back-off (level - 0.12 for 4 s) or, during one, only extends it;
    an arc drops the level again. probe: the labelling probes (see PROBE_EVERY_S). Returns the log and totals."""
    rng = np.random.default_rng(seed)
    level, cap_until, cap = p.v0 + 0.15, -1.0, 1.0
    off_until, recent = -1.0, []
    phase = rng.uniform(0, 2 * math.pi)
    arcs, actions, log_t, log_f, log_p, log_probe = [], [], [], [], [], []
    warnings = faults = 0
    capture = 0.0
    next_log = 0.0
    steps = int(minutes * 60 / dt)
    for k in range(steps):
        t = k * dt
        fog = 0.7 + 0.3 * math.sin(2 * math.pi * t / 600 + phase)
        if t < off_until:
            continue
        probing = probe and (t % probe_every_s) < probe_s
        limit = cap if t < cap_until else 0.95
        level = min(level + climb_per_s * dt, limit, 0.95)   # a probe climbs as usual: it explores the risky side
        capture += fog * (1 - math.exp(-6 * max(level - p.v0, 0))) * dt
        x = phys.window(p, level, fog, rng)
        z, sigma, median = cnn.preprocess(x[None, :])
        _, f = q.accumulate(cnn.quantize_input(z))
        if t >= next_log:
            log_t.append(t)
            log_f.append(f[0].copy())
            log_p.append(1 - math.exp(-phys.hazard(p, level, fog) * HORIZON_S))  # truth, for the evaluation only
            log_probe.append(probing)
            next_log = t + LOG_EVERY_S
        backoff = False
        if rng.random() < 1 - math.exp(-phys.hazard(p, level, fog) * dt):
            arcs.append(t)
            backoff = True
            recent = [a for a in recent if t - a <= arc_window_s] + [t]
            if len(recent) >= max_arcs:
                faults += 1
                actions.append(t)
                recent = []
                off_until, level, cap_until = t + rearm_s, p.v0 + 0.15, -1.0
                continue
        elif not probing and layer.risk(f)[0] > threshold:
            warnings += 1
            actions.append(t)
            if t < cap_until:                       # already backing off: the safety core only extends it
                cap_until = t + backoff_s
            else:
                level = max(p.v0 + 0.1, level - backoff_drop)
                cap, cap_until = level, t + backoff_s
        if backoff:                                 # an arc: drop again, whatever the state
            actions.append(t)
            level = max(p.v0 + 0.1, level - backoff_drop)
            cap, cap_until = level, t + backoff_s
    hours = minutes / 60
    log = {"t": np.asarray(log_t), "features": np.asarray(log_f, dtype=np.float64), "arcs": arcs,
           "true_p": np.asarray(log_p), "probe": np.asarray(log_probe),
           "actions": actions, "end": steps * dt}
    return log, {"arcs_per_h": len(arcs) / hours, "warnings_per_h": warnings / hours,
                 "faults_per_h": faults / hours, "water": capture / (steps * dt)}


def window_quality(p, q, layer, n=3000, seed=0):
    """Fresh windows of this node: AUC against sampled labels, and log loss against the TRUE probability
    (soft labels: no sampling noise), so small differences are measurable."""
    from .arc_eval import auc
    rng = np.random.default_rng(seed)
    X = np.empty((n, phys.WINDOW), np.float32)
    prob = np.empty(n)
    for i in range(n):
        v, fog = phys.draw_condition(p, rng)
        X[i] = phys.window(p, v, fog, rng)
        prob[i] = phys.arc_probability(p, v, fog)
    y = (rng.random(n) < prob).astype(np.int8)
    _, f = q.accumulate(cnn.quantize_input(cnn.preprocess(X)[0]))
    r = layer.risk(f)
    soft = float(-np.mean(prob * np.log(np.clip(r, 1e-6, 1)) + (1 - prob) * np.log(np.clip(1 - r, 1e-6, 1))))
    return {"auc": float(auc(r, y)), "soft_log_loss": soft}


# ---------------------------------------------------------------- the experiment

def buffered(log):
    """What the node still holds: its last BUFFER logged windows (and the events since the first of them)."""
    if len(log["t"]) <= BUFFER:
        return log
    t0 = log["t"][-BUFFER]
    return {"t": log["t"][-BUFFER:], "features": log["features"][-BUFFER:],
            "arcs": [a for a in log["arcs"] if a >= t0], "actions": [a for a in log["actions"] if a >= t0],
            "end": log["end"]}


def experiment(q, scale, seeds, rounds=6, round_min=20, eval_min=60, ridge=RIDGE, probes=True, log=print):
    """For each seed: three nodes, each in four versions run side by side on the same node and the same fog:
    factory (never learns), local (learns from its own windows every round), federated (every round the station
    averages the nodes' fits, each node gates the mean), naive (local, but without censoring: the bias).
    A round = round_min minutes of work with the layer of that version, then one adaptation. Afterwards every
    version is judged on the same fresh windows and the same closed loop (paired comparison)."""
    global RIDGE
    keep_ridge, RIDGE = RIDGE, ridge
    rows = []
    try:
        for r in seeds:
            factory = LastLayer.factory(q)
            names = list(NODES)
            ps = [node_params(10_000 * r + j, NODES[n]) for j, n in enumerate(names)]
            layer = {v: [factory] * len(names) for v in ("local", "fed", "naive")}
            store = {v: [(np.zeros((0, cnn.FEATURES)), np.zeros(0, np.int8), np.zeros(0, bool))] * len(names)
                     for v in layer}
            adopted = {v: [0] * len(names) for v in layer}
            labels = {v: [0] * len(names) for v in layer}
            arc_labels = {v: [0] * len(names) for v in layer}
            for k in range(rounds):
                fits = []
                for v in ("local", "fed", "naive"):
                    for j in range(len(names)):
                        lg, _ = closed_loop(ps[j], q, layer[v][j], round_min, seed=20_000 * r + 100 * k + j,
                                            probe=probes)
                        F, y, _, pr = auto_label(lg, censor=(v != "naive"), unbiased=(v != "naive"))
                        labels[v][j] += len(y)
                        arc_labels[v][j] += int(y.sum())
                        sF, sy, sp = store[v][j]                    # the node's store of useful windows
                        F, y, pr = (np.vstack([sF, F])[-BUFFER:], np.concatenate([sy, y])[-BUFFER:],
                                    np.concatenate([sp, pr])[-BUFFER:])
                        store[v][j] = (F, y, pr)
                        if v == "fed":
                            fits.append((F, y, pr))
                            continue
                        new, ok, _ = adapt_local(layer[v][j], F, y, scale, pr if v != "naive" else None)
                        layer[v][j] = new
                        adopted[v][j] += ok
                # federated round: each node fits on its windows (from its current layer), the station averages
                cands = [(fit(F, y, layer["fed"][j], scale), len(y)) for j, (F, y, _) in enumerate(fits)
                         if len(y) >= MIN_LABELS]
                if cands:
                    mean = fedavg([c for c, _ in cands], [n for _, n in cands])
                    for j, (F, y, pr) in enumerate(fits):
                        hold = np.zeros(len(y), bool)
                        hold[int(round(len(y) * (1 - HOLDOUT))):] = True
                        hold &= np.asarray(pr, bool)
                        m = shrink_to_trust(layer["fed"][j], mean, F)
                        if hold.sum() >= 20 and y[hold].sum() >= MIN_POS_HOLD and gate(layer["fed"][j], m, F[hold], y[hold]):
                            layer["fed"][j] = m
                            adopted["fed"][j] += 1
            for j, name in enumerate(names):
                row = {"seed": r, "node": name}
                for v in ("factory", "local", "fed", "naive"):
                    lay = factory if v == "factory" else layer[v][j]
                    wq = window_quality(ps[j], q, lay, seed=30_000 * r + j)
                    _, cl = closed_loop(ps[j], q, lay, eval_min, seed=40_000 * r + j)
                    row[v] = {**wq, **cl}
                    if v != "factory":
                        row[v].update(adopted=adopted[v][j], labels=labels[v][j], arc_labels=arc_labels[v][j])
                rows.append(row)
                log(f"  seed {r} {name:30} " + " | ".join(
                    f"{v[:5]} ll {row[v]['soft_log_loss']:.3f} arcs {row[v]['arcs_per_h']:3.0f} "
                    f"flt {row[v]['faults_per_h']:.0f} water {row[v]['water']:.3f}"
                    + (f" ad {row[v]['adopted']}" if v != "factory" else "")
                    for v in ("factory", "local", "fed", "naive")))
    finally:
        RIDGE = keep_ridge
    return rows
