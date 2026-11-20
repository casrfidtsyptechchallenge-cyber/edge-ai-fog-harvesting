"""Fog-front estimation (M8): direction and speed of a straight fog front from the nodes' detection times.

A straight front moving at constant speed reaches the point p at

    t(p) = t0 + w . p        w = (sin(direction), cos(direction)) / speed   ("slowness", s/m)

with x east and y north (the positions of the info messages). Each detection gives one equation, so:

- 3 or more nodes that are not on one line: least squares gives t0 and w, hence direction and speed;
- 2 nodes, or nodes on one line: only the slowness along the line is measured; the part across the line
  comes from the prior (the site's usual fog). Without a prior there is no estimate: two nodes cannot tell a
  front moving along their line from a faster one crossing it;
- 1 node: the prior alone, if there is one.

The prior is the site's usual fog front: set by hand (prevailing wind), or learned from the fronts already
fitted. Plain Python, no numpy: it runs on the Pi as is, and it is tested on its own
(tests/test_network_front.py).
"""

import math
from dataclasses import dataclass


def direction_of(wx, wy):
    """Degrees clockwise from north toward which the front moves, in [0, 360)."""
    deg = math.degrees(math.atan2(wx, wy)) % 360.0
    return 0.0 if deg >= 359.95 else deg


def slowness(direction_deg, speed_mps):
    th = math.radians(direction_deg)
    return (math.sin(th) / speed_mps, math.cos(th) / speed_mps)


def angle_error(a, b):
    """Smallest difference between two directions, degrees (0 to 180)."""
    d = abs(a - b) % 360.0
    return min(d, 360.0 - d)


def _solve3(a, b):
    """Solves the 3x3 system a.x = b (Gaussian elimination with pivoting). None if it is singular."""
    m = [list(a[i]) + [b[i]] for i in range(3)]
    for col in range(3):
        pivot = max(range(col, 3), key=lambda r: abs(m[r][col]))
        if abs(m[pivot][col]) < 1e-12:
            return None
        m[col], m[pivot] = m[pivot], m[col]
        for r in range(3):
            if r != col:
                f = m[r][col] / m[col][col]
                m[r] = [m[r][k] - f * m[col][k] for k in range(4)]
    return [m[i][3] / m[i][i] for i in range(3)]


def _principal_axes(points):
    """Unit vector of the line the points spread along most, and the spread across it (m, standard deviation)."""
    n = len(points)
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    sxx = sum((p[0] - mx) ** 2 for p in points) / n
    syy = sum((p[1] - my) ** 2 for p in points) / n
    sxy = sum((p[0] - mx) * (p[1] - my) for p in points) / n
    half_trace, det = (sxx + syy) / 2, sxx * syy - sxy * sxy
    root = math.sqrt(max(0.0, half_trace * half_trace - det))
    big, small = half_trace + root, max(0.0, half_trace - root)
    if abs(sxy) > 1e-12:
        ux, uy = big - syy, sxy
    else:
        ux, uy = (1.0, 0.0) if sxx >= syy else (0.0, 1.0)
    norm = math.hypot(ux, uy)
    return (ux / norm, uy / norm), math.sqrt(small)


@dataclass
class Fit:
    """A front estimate. basis says what it rests on:
    "fit"   3+ nodes not on one line: measured
    "line"  2 nodes (or nodes on one line): the speed along the line is measured, the rest is the prior
    "prior" 1 node: the site's usual front, placed at this detection
    """
    t0: float                 # s, on the same clock as the detection times
    wx: float                 # slowness, s/m
    wy: float
    basis: str
    quality: object = None    # 0..1 (R² of the arrival times) with 4+ nodes; None when nothing can be checked
    nodes: int = 0

    @property
    def speed_mps(self):
        w = math.hypot(self.wx, self.wy)
        return None if w < 1e-9 else 1.0 / w

    @property
    def direction_deg(self):
        return None if self.speed_mps is None else direction_of(self.wx, self.wy)

    def arrival(self, x, y):
        return self.t0 + self.wx * x + self.wy * y


@dataclass
class FitSettings:
    min_spread_m: float = 2.0     # nodes closer than this to one line count as on one line
    max_speed_mps: float = 50.0   # faster: fog everywhere at once, not a moving front


def fit_front(detections, prior=None, settings=FitSettings()):
    """detections: [(x_m, y_m, t_s)] for the nodes that saw the fog, any order. prior: (wx, wy) or None.

    Returns a Fit, or None when nothing can be said (fewer than 3 nodes off one line, and no prior). A Fit
    whose speed is None means the fog came everywhere at once (no direction).
    """
    n = len(detections)
    if n == 0:
        return None
    t_ref = min(d[2] for d in detections)
    pts = [(x, y, t - t_ref) for x, y, t in detections]
    if n == 1:
        if prior is None:
            return None
        x, y, t = pts[0]
        return Fit(t_ref + t - prior[0] * x - prior[1] * y, prior[0], prior[1], "prior", None, 1)

    axis, spread = _principal_axes([(x, y) for x, y, _ in pts])
    if n >= 3 and spread >= settings.min_spread_m:
        ata = [[0.0] * 3 for _ in range(3)]
        atb = [0.0] * 3
        for x, y, t in pts:
            row = (1.0, x, y)
            for i in range(3):
                atb[i] += row[i] * t
                for j in range(3):
                    ata[i][j] += row[i] * row[j]
        sol = _solve3(ata, atb)
        if sol is not None:
            t0, wx, wy = sol
            quality = None
            if n >= 4:
                mean_t = sum(t for _, _, t in pts) / n
                ss_tot = sum((t - mean_t) ** 2 for _, _, t in pts)
                ss_res = sum((t - (t0 + wx * x + wy * y)) ** 2 for x, y, t in pts)
                quality = None if ss_tot < 1e-9 else max(0.0, min(1.0, 1.0 - ss_res / ss_tot))
            return _limited(Fit(t_ref + t0, wx, wy, "fit", quality, n), pts, t_ref, settings)

    if prior is None:
        return None
    # Along the line only: t = t0 + s * (p . axis); across the line, the prior
    ux, uy = axis
    s_vals = [x * ux + y * uy for x, y, _ in pts]
    ms = sum(s_vals) / n
    mt = sum(t for _, _, t in pts) / n
    var = sum((s - ms) ** 2 for s in s_vals)
    s_par = 0.0 if var < 1e-9 else sum((s - ms) * (t - mt) for s, (_, _, t) in zip(s_vals, pts)) / var
    vx, vy = -uy, ux  # across the line
    across = prior[0] * vx + prior[1] * vy
    wx, wy = s_par * ux + across * vx, s_par * uy + across * vy
    t0 = mt - sum(wx * x + wy * y for x, y, _ in pts) / n
    return _limited(Fit(t_ref + t0, wx, wy, "line", None, n), pts, t_ref, settings)


def _limited(fit, pts, t_ref, settings):
    """Too fast to be a moving front (the nodes saw it at the same time): no direction; the fog is due
    everywhere at the mean detection time."""
    if fit.speed_mps is not None and fit.speed_mps > settings.max_speed_mps:
        mean_t = sum(t for _, _, t in pts) / len(pts)
        return Fit(t_ref + mean_t, 0.0, 0.0, fit.basis, fit.quality, fit.nodes)
    return fit