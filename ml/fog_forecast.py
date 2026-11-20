"""Fog nowcast from real airport observations (METAR): will fog form here in the next 1 to 3 hours?

The arc predictor sees fog arrive (M8 tracks fronts between nodes); this model sees it FORM, from what one node
measures anyway: temperature and humidity (SHT45). The station can then pre-arm the nodes before the fog, and
plan its battery. It is the first model of the project trained and tested on real data.

Data: the Iowa Environmental Mesonet METAR archive (network "Tunisia ASOS"), no account needed:
  https://mesonet.agron.iastate.edu/request/download.phtml  -> Tunisia ASOS, one or more stations,
  variables: tmpc, dwpc, relh, sknt, drct, vsby; timezone UTC; format "Comma Delimited, no debug headers"
  (or the "onlycomma" option); several years.
Then:  python -m ml fog-forecast data/metar/*.csv

Method (written down before any result, so the test is not tuned on):
  * hourly grid per station (the last report in each hour);
  * fog = visibility < 1 km (0.62 mile, the METAR "FG" definition);
  * target: fog starts within the next H hours (H = 1 and 3), asked only when there is no fog now (onset, the
    useful and harder question; "fog continues" is trivial);
  * features only from the past and present (no leakage), and only what a node measures: temperature, dew
    point depression (from humidity), their changes over 1 h and 3 h, hour of day and day of year. The wind
    (not measured on a node) is added in a second model only, to price an anemometer;
  * logistic regression (numpy, standardised features, L2), trained on every year but the last, tested on the
    last year (a whole unseen year: the honest split for weather);
  * baselines: climatology (the training rate by hour and month) and the forecaster's rule of thumb
    "dew point depression <= 1 °C and wind <= 3 kt".
"""
import csv
import json
import math
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

FOG_MILES = 0.62           # 1 km
HORIZONS_H = (1, 3)
L2 = 1.0
BASE_FEATURES = ["t_c", "spread_c", "d_spread_1h", "d_spread_3h", "d_t_1h", "d_t_3h",
                 "hour_sin", "hour_cos", "doy_sin", "doy_cos"]
WIND_FEATURES = ["wind_kt", "d_wind_3h"]


# ---------------------------------------------------------------- reading

def _num(v):
    try:
        x = float(v)
        return x if math.isfinite(x) else None
    except (TypeError, ValueError):
        return None


def read_iem(paths):
    """IEM ASOS CSV files -> {station: [(datetime UTC, dict)]}, sorted. Accepts 'M' or '' for missing."""
    out = defaultdict(list)
    for path in paths:
        with open(path, newline="", encoding="utf-8") as f:
            lines = (ln for ln in f if not ln.startswith("#"))
            for row in csv.DictReader(lines):
                try:
                    t = datetime.strptime(row["valid"].strip(), "%Y-%m-%d %H:%M").replace(tzinfo=timezone.utc)
                except (KeyError, ValueError):
                    continue
                tc, dc, rh = _num(row.get("tmpc")), _num(row.get("dwpc")), _num(row.get("relh"))
                if tc is None and _num(row.get("tmpf")) is not None:
                    tc = (_num(row["tmpf"]) - 32) / 1.8
                if dc is None and _num(row.get("dwpf")) is not None:
                    dc = (_num(row["dwpf"]) - 32) / 1.8
                out[row.get("station", "?").strip()].append((t, {
                    "t_c": tc, "dew_c": dc, "rh": rh, "wind_kt": _num(row.get("sknt")),
                    "vsby_mi": _num(row.get("vsby"))}))
    for st in out:
        out[st].sort(key=lambda r: r[0])
    return dict(out)


def dew_point(t_c, rh):
    """Magnus formula: what the node computes from the SHT45 (temperature, humidity)."""
    a, b = 17.62, 243.12
    g = math.log(max(rh, 1e-3) / 100.0) + a * t_c / (b + t_c)
    return b * g / (a - g)


def hourly(rows):
    """The last report of each hour -> arrays on a regular hourly grid (missing hours are NaN)."""
    by_hour = {}
    for t, r in rows:
        by_hour[t.replace(minute=0, second=0, microsecond=0)] = r
    if not by_hour:
        return None
    start, end = min(by_hour), max(by_hour)
    n = int((end - start).total_seconds() // 3600) + 1
    grid = {k: np.full(n, np.nan) for k in ("t_c", "dew_c", "wind_kt", "vsby_mi")}
    times = np.array([start.timestamp() + 3600 * i for i in range(n)])
    for t, r in by_hour.items():
        i = int((t - start).total_seconds() // 3600)
        dew = r["dew_c"]
        if dew is None and r["t_c"] is not None and r["rh"] is not None:
            dew = dew_point(r["t_c"], r["rh"])
        for k, v in (("t_c", r["t_c"]), ("dew_c", dew), ("wind_kt", r["wind_kt"]), ("vsby_mi", r["vsby_mi"])):
            if v is not None:
                grid[k][i] = v
    grid["time"] = times
    return grid


# ---------------------------------------------------------------- features and target

def _lag(x, h):
    out = np.full_like(x, np.nan)
    out[h:] = x[:-h]
    return out


def build(grid, horizon_h):
    """Rows where the target is defined: no fog now, and visibility known for the next horizon_h hours.
    Features use only hours <= now."""
    t, dew, wind, vis = grid["t_c"], grid["dew_c"], grid["wind_kt"], grid["vsby_mi"]
    spread = t - dew
    fog = vis < FOG_MILES
    known = ~np.isnan(vis)
    n = len(t)
    future_fog = np.zeros(n, bool)
    future_known = np.ones(n, bool)
    for k in range(1, horizon_h + 1):
        f = np.zeros(n, bool)
        f[:-k] = fog[k:]
        kn = np.zeros(n, bool)
        kn[:-k] = known[k:]
        future_fog |= f
        future_known &= kn
    dt = [datetime.fromtimestamp(s, timezone.utc) for s in grid["time"]]
    hour = np.array([d.hour for d in dt], float)
    doy = np.array([d.timetuple().tm_yday for d in dt], float)
    X = {
        "t_c": t, "spread_c": spread,
        "d_spread_1h": spread - _lag(spread, 1), "d_spread_3h": spread - _lag(spread, 3),
        "d_t_1h": t - _lag(t, 1), "d_t_3h": t - _lag(t, 3),
        "hour_sin": np.sin(2 * np.pi * hour / 24), "hour_cos": np.cos(2 * np.pi * hour / 24),
        "doy_sin": np.sin(2 * np.pi * doy / 365.25), "doy_cos": np.cos(2 * np.pi * doy / 365.25),
        "wind_kt": wind, "d_wind_3h": wind - _lag(wind, 3),
    }
    ok = known & ~fog & future_known
    for k in BASE_FEATURES + WIND_FEATURES:
        ok &= ~np.isnan(X[k])
    year = np.array([d.year for d in dt])
    month = np.array([d.month for d in dt])
    return {k: v[ok] for k, v in X.items()}, future_fog[ok].astype(np.int8), year[ok], hour[ok].astype(int), \
        month[ok]


# ---------------------------------------------------------------- model

class Logistic:
    def __init__(self, names):
        self.names = list(names)

    def matrix(self, X):
        return np.column_stack([X[k] for k in self.names])

    def fit(self, X, y, steps=50):
        A = self.matrix(X)
        self.mu, self.sd = A.mean(axis=0), A.std(axis=0) + 1e-9
        Z = np.hstack([(A - self.mu) / self.sd, np.ones((len(y), 1))])
        w = np.zeros(Z.shape[1])
        w[-1] = math.log(max(y.mean(), 1e-4) / max(1 - y.mean(), 1e-4))
        reg = np.full(Z.shape[1], L2)
        reg[-1] = 0.0
        n = len(y)
        for _ in range(steps):  # Newton (IRLS) with L2, backtracking
            p = 1 / (1 + np.exp(-np.clip(Z @ w, -30, 30)))
            g = Z.T @ (p - y) / n + reg * w / n
            H = (Z * (p * (1 - p))[:, None]).T @ Z / n + np.diag(reg / n + 1e-9)
            step = np.linalg.solve(H, g)
            obj = lambda v: np.mean(np.logaddexp(0, Z @ v) - y * (Z @ v)) + 0.5 * np.sum(reg * v * v) / n
            a, f0 = 1.0, obj(w)
            while a > 1e-4 and obj(w - a * step) >= f0:
                a *= 0.5
            if a <= 1e-4:
                break
            w = w - a * step
            if np.abs(a * step).max() < 1e-8:
                break
        self.w = w
        return self

    def predict(self, X):
        Z = (self.matrix(X) - self.mu) / self.sd
        return 1 / (1 + np.exp(-np.clip(Z @ self.w[:-1] + self.w[-1], -30, 30)))

    def to_dict(self):
        return {"features": self.names, "mean": self.mu.tolist(), "scale": self.sd.tolist(),
                "weights": self.w[:-1].tolist(), "bias": float(self.w[-1])}


def auc(p, y):
    order = np.argsort(p)
    ranks = np.empty(len(p))
    ranks[order] = np.arange(1, len(p) + 1)
    pos = y == 1
    n1, n0 = pos.sum(), (~pos).sum()
    if n1 == 0 or n0 == 0:
        return float("nan")
    return float((ranks[pos].sum() - n1 * (n1 + 1) / 2) / (n1 * n0))


def average_precision(p, y):
    order = np.argsort(-p)
    ys = y[order]
    tp = np.cumsum(ys)
    prec = tp / np.arange(1, len(ys) + 1)
    return float((prec * ys).sum() / max(ys.sum(), 1))


def brier(p, y):
    return float(np.mean((p - y) ** 2))


def scores(p, y):
    return {"auc": auc(p, y), "average_precision": average_precision(p, y), "brier": brier(p, y),
            "base_rate": float(y.mean())}


def run(paths, out_json=None, log=print):
    """Train and test per horizon on all stations pooled; returns the report."""
    stations = read_iem(paths)
    grids = {s: hourly(r) for s, r in stations.items() if r}
    report = {"stations": sorted(grids), "horizons": {}}
    for H in HORIZONS_H:
        parts = [build(g, H) for g in grids.values() if g is not None]
        X = {k: np.concatenate([p[0][k] for p in parts]) for k in parts[0][0]}
        y = np.concatenate([p[1] for p in parts])
        year = np.concatenate([p[2] for p in parts])
        hour = np.concatenate([p[3] for p in parts])
        month = np.concatenate([p[4] for p in parts])
        last = int(year.max())
        tr, te = year < last, year == last
        if tr.sum() == 0 or te.sum() == 0 or y[te].sum() == 0:
            raise ValueError("need at least two years with fog in the last one")
        Xtr = {k: v[tr] for k, v in X.items()}
        Xte = {k: v[te] for k, v in X.items()}
        node = Logistic(BASE_FEATURES).fit(Xtr, y[tr])
        windy = Logistic(BASE_FEATURES + WIND_FEATURES).fit(Xtr, y[tr])
        clim = defaultdict(lambda: [0, 0])
        for hh, mm, yy in zip(hour[tr], month[tr], y[tr]):
            clim[(hh, mm)][0] += yy
            clim[(hh, mm)][1] += 1
        p_clim = np.array([(clim[(hh, mm)][0] + 0.5) / (clim[(hh, mm)][1] + 1) for hh, mm in zip(hour[te], month[te])])
        rule = ((Xte["spread_c"] <= 1.0) & (Xte["wind_kt"] <= 3.0)).astype(float)
        res = {"train_rows": int(tr.sum()), "test_rows": int(te.sum()), "test_year": last,
               "fog_onsets_in_test": int(y[te].sum()),
               "node model (T, RH, time)": scores(node.predict(Xte), y[te]),
               "with wind (needs an anemometer)": scores(windy.predict(Xte), y[te]),
               "climatology (hour x month)": scores(p_clim, y[te]),
               "rule: spread <= 1 C and wind <= 3 kt": scores(rule, y[te]),
               "model": node.to_dict()}
        report["horizons"][f"{H}h"] = res
        log(f"fog within {H} h (no fog now): train {res['train_rows']} h, test {res['test_rows']} h of {last}, "
            f"{res['fog_onsets_in_test']} onsets")
        for name in ("node model (T, RH, time)", "with wind (needs an anemometer)", "climatology (hour x month)",
                     "rule: spread <= 1 C and wind <= 3 kt"):
            s = res[name]
            log(f"  {name:38} AUC {s['auc']:.3f}  AP {s['average_precision']:.3f}  Brier {s['brier']:.4f}")
    if out_json:
        Path(out_json).write_text(json.dumps(report, indent=1), encoding="utf-8")
    return report
