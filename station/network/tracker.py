"""The fog-front tracker (M8): one front at a time, from the first FOG_DETECTED until the fog has passed.

For each front it keeps the detections (node, time), fits the front after each one (front.py), predicts when
the fog reaches the other nodes, and decides which nodes to pre-arm:

- a node is pre-armed if it is ARMED and the fog has not reached it yet;
- PREARM keeps a node ready until eta + 60 s (the contract), so it is sent again only when the expected
  arrival moves past that window;
- with no direction yet (1 node and no prior, or 2 nodes and no prior), the expected arrival is the distance
  from the nearest node that saw the fog, at a slow speed: the window is long rather than too short.

The site's usual front (the prior) is set by hand or learned: each front measured on 3+ nodes moves it. It
is used only while the fronts agree on a direction (at a site with a prevailing wind); when they come from
anywhere, it is not used and the pre-arming falls back on distance.
Plain logic on given times, with no clock and no network: tested on its own (tests/test_network_front.py).
"""

import math
from dataclasses import dataclass, field

import aeroion_contract as ac

from .front import FitSettings, Fit, direction_of, fit_front

PREARM_WINDOW_S = 60.0   # a PREARM keeps the node ready until eta_s + 60 s (cmd.schema.json)
MAX_PREARM_ETA_S = 3600.0


@dataclass
class TrackerSettings:
    fit: FitSettings = field(default_factory=FitSettings)
    slow_speed_mps: float = 0.5       # with no direction yet: the fog may come this slowly
    publish_every_s: float = 10.0     # the front message, while a front is tracked (the contract)
    end_grace_s: float = 60.0         # after the last expected arrival, once every node that saw it has cleared
    max_front_s: float = 1800.0       # a front lasting longer is closed anyway
    max_detection_age_s: float = 300.0  # an older detection (replayed late) does not start a front
    prearm: bool = True
    prearm_margin_s: float = 15.0     # send again when the arrival moves past the covered window minus this
    prearm_min_gap_s: float = 5.0     # at most one PREARM per node per 5 s
    learn_rate: float = 0.3           # how far each measured front moves the learned prior
    min_agreement: float = 0.7        # the prior is used only while the fronts' directions agree this well
    min_fronts: int = 3               # ... and, when learned, once it rests on this many fronts
    keep_finished: int = 50           # finished fronts kept in memory (the live service runs for weeks)


@dataclass
class Prior:
    """The site's usual front: a direction (as a mean of unit vectors) and a speed.

    agreement is the length of the mean unit vector: 1 when every front came the same way, about 0 when
    they came from everywhere. A prior set by hand starts at 1.
    """
    ux: float
    uy: float
    speed_mps: float
    fronts: int = 0
    source: str = "set"   # "set" (by hand) or "learned"

    @classmethod
    def toward(cls, direction_deg, speed_mps, source="set", fronts=0):
        th = math.radians(direction_deg)
        return cls(math.sin(th), math.cos(th), float(speed_mps), fronts, source)

    @property
    def agreement(self):
        return math.hypot(self.ux, self.uy)

    @property
    def direction_deg(self):
        return direction_of(self.ux, self.uy)

    def slowness(self, min_agreement, min_fronts=0):
        """(wx, wy) for the fit, or None while the fronts do not agree on a direction (or, learned, while it
        rests on fewer than min_fronts fronts)."""
        r = self.agreement
        if r < min_agreement or self.speed_mps <= 0 or (self.source == "learned" and self.fronts < min_fronts):
            return None
        return (self.ux / r / self.speed_mps, self.uy / r / self.speed_mps)

    def learn(self, direction_deg, speed_mps, rate):
        th = math.radians(direction_deg)
        self.ux = (1 - rate) * self.ux + rate * math.sin(th)
        self.uy = (1 - rate) * self.uy + rate * math.cos(th)
        self.speed_mps = (1 - rate) * self.speed_mps + rate * speed_mps
        self.fronts += 1

    def as_dict(self):
        return {"ux": self.ux, "uy": self.uy, "speed_mps": self.speed_mps, "fronts": self.fronts,
                "source": self.source}

    @classmethod
    def from_dict(cls, d):
        return cls(float(d["ux"]), float(d["uy"]), float(d["speed_mps"]), int(d.get("fronts", 0)),
                   str(d.get("source", "learned")))


@dataclass
class NodePlace:
    x: float = None
    y: float = None
    synced: object = None   # None: not known yet
    state: str = None
    online: bool = True

    @property
    def placed(self):
        return self.x is not None and self.y is not None


@dataclass
class Front:
    started_ms: int
    detections: dict = field(default_factory=dict)   # node -> detection ts (ms), in order
    cleared: set = field(default_factory=set)
    fit: Fit = None
    expected: dict = field(default_factory=dict)     # node -> expected arrival (ms) for the nodes not reached yet
    published: dict = field(default_factory=dict)    # node -> eta in the front message (only with a direction)
    prearms: dict = field(default_factory=dict)      # node -> [(sent_ms, eta_s)]
    not_armed: set = field(default_factory=set)      # nodes the fog was coming to that were not ARMED
    estimates: list = field(default_factory=list)    # [(now_ms, nodes, basis, direction, speed, {node: arrival_ms})]
    last_publish_ms: int = 0

    @property
    def source(self):
        return next(iter(self.detections))


class FrontTracker:
    def __init__(self, settings=None, prior=None):
        self.s = settings or TrackerSettings()
        self.prior = prior                  # a Prior, or None
        self.places = {}
        self.front = None
        self.finished = []                  # the fronts already closed (for scoring and the log)

    # ------------------------------------------------------------------ what is known about the nodes

    def place(self, node):
        return self.places.setdefault(node, NodePlace())

    def set_position(self, node, x, y):
        p = self.place(node)
        p.x, p.y = float(x), float(y)

    def set_synced(self, node, synced):
        self.place(node).synced = synced

    def set_state(self, node, state):
        self.place(node).state = state

    def set_online(self, node, online):
        p = self.place(node)
        p.online = online
        if not online:
            p.state = None

    # ------------------------------------------------------------------ events

    def detected(self, node, ts_ms, now_ms):
        """FOG_DETECTED from a node. Returns outputs: ("front", message), ("prearm", node, eta_s, source),
        ("log", text)."""
        p = self.place(node)
        if not p.placed:
            return [("log", f"{node} saw fog but its position is not known (no info message): not used")]
        if p.synced is False:
            return [("log", f"{node} saw fog but its clock is not synced: not used for the front")]
        if self.front is None:
            if now_ms - ts_ms > self.s.max_detection_age_s * 1000:
                return [("log", f"{node}: fog detection {int((now_ms - ts_ms) / 1000)} s old (replayed): not a new front")]
            self.front = Front(started_ms=ts_ms)
        elif node in self.front.detections:
            return []  # it saw this front already (fog came and went at that node)
        self.front.detections[node] = ts_ms
        out = [("log", f"fog at {node} ({len(self.front.detections)} node(s) so far)")]
        out += self._update(now_ms, publish=True)
        return out

    def cleared(self, node, ts_ms, now_ms):
        if self.front is not None and node in self.front.detections:
            self.front.cleared.add(node)
        return []

    def tick(self, now_ms):
        """Call every second or so: the periodic front message, PREARMs to refresh, the end of the front."""
        f = self.front
        if f is None:
            return []
        reached_all = set(f.detections) <= f.cleared
        last_expected = max(f.expected.values(), default=0)
        if (reached_all and now_ms > max(last_expected, max(f.detections.values())) + self.s.end_grace_s * 1000) \
                or now_ms - f.started_ms > self.s.max_front_s * 1000:
            return self._end(now_ms)
        return self._update(now_ms, publish=now_ms - f.last_publish_ms >= self.s.publish_every_s * 1000)

    # ------------------------------------------------------------------ inside

    def _update(self, now_ms, publish):
        f = self.front
        dets = [(self.places[n].x, self.places[n].y, ts / 1000) for n, ts in f.detections.items()]
        prior = self.prior.slowness(self.s.min_agreement, self.s.min_fronts) if self.prior else None
        f.fit = fit_front(dets, prior, self.s.fit)
        f.expected = self._expected(now_ms)
        f.published = {}
        if f.fit is not None and f.fit.direction_deg is not None:
            f.published = {n: max(0.0, (t - now_ms) / 1000) for n, t in f.expected.items()}
        elif f.fit is not None:  # everywhere at once
            f.published = {n: 0.0 for n in f.expected}
        f.estimates.append((now_ms, len(f.detections), f.fit.basis if f.fit else "distance",
                            f.fit.direction_deg if f.fit else None, f.fit.speed_mps if f.fit else None,
                            dict(f.expected)))
        out = []
        if self.s.prearm:
            out += self._prearms(now_ms)
        if publish:
            f.last_publish_ms = now_ms
            out.append(("front", self.message(now_ms)))
        return out

    def _expected(self, now_ms):
        """Expected arrival (ms) at every placed node the fog has not reached."""
        f = self.front
        out = {}
        reached = [(self.places[n].x, self.places[n].y, ts) for n, ts in f.detections.items()]
        for node, p in sorted(self.places.items()):
            if node in f.detections or not p.placed or not p.online:
                continue
            if f.fit is not None:
                out[node] = int(f.fit.arrival(p.x, p.y) * 1000)
            else:  # no direction yet: from the nearest node that saw it, at a slow speed
                x, y, ts = min(reached, key=lambda r: math.hypot(r[0] - p.x, r[1] - p.y))
                out[node] = int(ts + math.hypot(x - p.x, y - p.y) / self.s.slow_speed_mps * 1000)
        return out

    def _prearms(self, now_ms):
        f = self.front
        out = []
        for node, arrival_ms in f.expected.items():
            if self.places[node].state != "ARMED":
                f.not_armed.add(node)
                continue  # only an ARMED node can be pre-armed (LOCKED, SAFE...: the safety core says no)
            eta = min(MAX_PREARM_ETA_S, max(0.0, (arrival_ms - now_ms) / 1000))
            sent = f.prearms.get(node)
            if sent:
                last_ms, last_eta = sent[-1]
                covered_until = last_ms + (last_eta + PREARM_WINDOW_S) * 1000
                if arrival_ms <= covered_until - self.s.prearm_margin_s * 1000:
                    continue  # still covered
                if now_ms - last_ms < self.s.prearm_min_gap_s * 1000:
                    continue
            f.prearms.setdefault(node, []).append((now_ms, round(eta, 1)))
            out.append(("prearm", node, round(eta, 1), f.source))
        return out

    def message(self, now_ms, active=True):
        """The front message (front.schema.json)."""
        f = self.front
        fit = f.fit
        direction = fit.direction_deg if fit else None
        return {
            "schema_version": ac.SCHEMA_VERSION,
            "ts": int(now_ms),
            "active": active,
            "direction_deg": None if direction is None else round(direction, 1) % 360.0,
            "speed_mps": None if fit is None or fit.speed_mps is None else round(fit.speed_mps, 2),
            "fit_quality": None if fit is None or fit.quality is None else round(fit.quality, 2),
            "nodes_detected": list(f.detections),
            "eta_s": {n: round(e, 1) for n, e in sorted(f.published.items())} if active else {},
        }

    def _end(self, now_ms):
        f = self.front
        out = [("front", self.message(now_ms, active=False))]
        fit = f.fit
        summary = (f"front over: {len(f.detections)} node(s), "
                   + (f"moving toward {fit.direction_deg:.0f}° at {fit.speed_mps:.2f} m/s ({fit.basis})"
                      if fit and fit.direction_deg is not None else "no direction measured"))
        out.append(("log", summary))
        if fit is not None and fit.basis == "fit" and fit.speed_mps is not None:
            if self.prior is None:
                self.prior = Prior.toward(fit.direction_deg, fit.speed_mps, source="learned", fronts=1)
            else:
                self.prior.learn(fit.direction_deg, fit.speed_mps, self.s.learn_rate)
            out.append(("learned", self.prior))
        self.finished.append(f)
        del self.finished[:-self.s.keep_finished]
        self.front = None
        return out