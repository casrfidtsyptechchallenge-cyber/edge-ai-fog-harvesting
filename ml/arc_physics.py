"""Synthetic collector-current windows for the arc predictor (M5a), until chamber data exists.

Real windows come from the nodes ("wave" messages, M2 windows.csv) once the HV bench runs; M11 retrains on
them. Until then, this generator stands in. What it assumes, from corona physics (to be checked on the bench):

- the current has a baseline I0 = K·V·(V − V0)·(1 − 0.45·fog) (Townsend law, lowered by fog), plus white
  noise, mains ripple from the HV supply and a slow drift;
- as the level V comes close to the sparkover level V_arc (lower in dense fog), short pre-discharge pulses
  appear, more often and larger the closer it is: rate = rate_max·exp(−gap/pulse_w), gap = V_arc − V;
- isolated interference spikes happen at any level: they must not raise the risk;
- arcs happen at random, at a rate that grows the same way but over a narrower band:
  hazard = hazard_max·exp(−gap/hazard_w) arcs per second. The pulses appear before the arcs become likely:
  that is what makes a warning possible.

Each "run" draws its own electrode, noise and sparkover level: a model is judged on runs it never saw.
"""

import math
from dataclasses import dataclass

import numpy as np

FS_HZ = 10_000      # samples per second of a window (the MCX captures at this rate)
WINDOW = 256        # samples per window: 25.6 ms
HORIZON_S = 5.0     # label: an arc within this many seconds (M2 label_arc_next, arc_horizon_s)
FOG_DROP = 0.45     # dense fog lowers the corona current by this fraction (as in sim/fake_node.py)


@dataclass
class RunParams:
    k: float              # Townsend coefficient, µA per level²
    v0: float             # corona onset, level
    v_arc: float          # sparkover level in clear air
    fog_shift: float      # dense fog lowers the sparkover level by this much
    noise_rel: float      # white noise, fraction of I0
    noise_abs: float      # white noise floor, µA
    ripple_rel: float     # mains ripple, fraction of I0
    pulse_rate_max: float  # pre-discharge pulses per second at the sparkover level
    pulse_w: float        # the pulse rate falls by e every pulse_w below sparkover
    pulse_amp: float      # pulse height, fraction of I0, far from sparkover
    pulse_tau: float      # pulse decay, samples
    emi_rate: float       # interference spikes per second, at any level
    hazard_max: float     # arcs per second at the sparkover level
    hazard_w: float       # the arc rate falls by e every hazard_w below sparkover


def draw_params(rng):
    """One run: a node with its own electrode, wiring noise and chamber."""
    return RunParams(
        k=rng.uniform(130, 190), v0=rng.uniform(0.19, 0.25), v_arc=rng.uniform(0.78, 0.90),
        fog_shift=rng.uniform(0.08, 0.16), noise_rel=rng.uniform(0.015, 0.045), noise_abs=rng.uniform(0.2, 0.6),
        ripple_rel=rng.uniform(0.0, 0.04), pulse_rate_max=rng.uniform(1200, 3500), pulse_w=rng.uniform(0.025, 0.04),
        pulse_amp=rng.uniform(0.04, 0.12), pulse_tau=rng.uniform(1.5, 4.0), emi_rate=rng.uniform(2, 25),
        hazard_max=rng.uniform(0.6, 1.6), hazard_w=rng.uniform(0.012, 0.02))


def sparkover(p, fog):
    return p.v_arc - p.fog_shift * fog


def baseline_ua(p, v, fog):
    return p.k * v * max(v - p.v0, 0.0) * (1 - FOG_DROP * fog)


def hazard(p, v, fog):
    """Arcs per second at level v (capped: past sparkover an arc comes within a fraction of a second)."""
    gap = sparkover(p, fog) - v
    return p.hazard_max * math.exp(-max(gap, -0.06) / p.hazard_w)


def arc_probability(p, v, fog, horizon_s=HORIZON_S):
    """Chance of an arc within horizon_s if the level stays at v: the predictor's target."""
    return 1.0 - math.exp(-hazard(p, v, fog) * horizon_s)


def window(p, v, fog, rng, n=WINDOW, fs=FS_HZ):
    """One window of collector current, µA."""
    i0 = baseline_ua(p, v, fog)
    t = np.arange(n) / fs
    x = i0 + rng.normal(0.0, p.noise_rel * i0 + p.noise_abs, n)
    for hz in (50.0, 100.0):
        x += p.ripple_rel * i0 * rng.uniform(0.3, 1.0) * np.sin(2 * np.pi * hz * t + rng.uniform(0, 2 * np.pi))
    x += i0 * rng.normal(0.0, 0.01) * (t / t[-1] - 0.5)          # slow drift across the window
    gap = sparkover(p, fog) - v
    closeness = math.exp(-max(gap, -0.06) / p.pulse_w)          # 1 at sparkover
    tail = np.exp(-np.arange(n) / p.pulse_tau)
    for k0 in rng.integers(0, n, rng.poisson(p.pulse_rate_max * closeness * n / fs)):
        amp = i0 * p.pulse_amp * (1 + 2 * closeness) * rng.lognormal(0.0, 0.5)
        x[k0:] += amp * tail[:n - k0]
    for k0 in rng.integers(0, n, rng.poisson(p.emi_rate * n / fs)):
        x[k0] += i0 * rng.uniform(0.1, 0.6) * (1 if rng.random() < 0.8 else -1)
    return np.maximum(x, 0.0)


def draw_condition(p, rng):
    """A level and a fog density as the node meets them: mostly far from sparkover, often close to it."""
    while True:
        fog = rng.uniform(0.3, 1.0)
        gap = rng.uniform(0.0, 0.35) if rng.random() < 0.6 else rng.uniform(-0.02, 0.09)
        v = sparkover(p, fog) - gap
        if v > p.v0 + 0.08 and v <= 0.98:
            return v, fog


def make_dataset(n_runs, windows_per_run, seed):
    """Windows from n_runs runs. Returns X (N, WINDOW) float32 µA, y (N,) 0/1 (an arc followed within the
    horizon, drawn at random as it would happen), and meta (N, 4): run, level, fog, true probability."""
    rng = np.random.default_rng(seed)
    X = np.empty((n_runs * windows_per_run, WINDOW), dtype=np.float32)
    y = np.empty(n_runs * windows_per_run, dtype=np.int8)
    meta = np.empty((n_runs * windows_per_run, 4))
    i = 0
    for run in range(n_runs):
        p = draw_params(rng)
        for _ in range(windows_per_run):
            v, fog = draw_condition(p, rng)
            prob = arc_probability(p, v, fog)
            X[i] = window(p, v, fog, rng)
            y[i] = rng.random() < prob
            meta[i] = (run, v, fog, prob)
            i += 1
    return X, y, meta