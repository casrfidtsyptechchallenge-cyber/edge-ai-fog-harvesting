"""The water point (M9, F13): who may draw water, how much, and when the valve closes.

A household card starts a session if the water is safe, a filter is fitted and not used up, and the card
has quota left today. The valve is open exactly while a session runs. The session ends, and the valve
closes, when:

    quota    the card's quota for today is reached
    card     the same card is tapped again
    idle     no water flowed for idle_s (the person left, or the tank is empty)
    timeout  the session lasted max_session_s
    quality  the quality verdict is no longer ok
    filter   the filter reached its capacity
    stop     the water point was stopped

Filter change: a technician (or admin) taps their badge, then, within filter_change_s, the new cartridge's
tag. Only a tag registered by the admin is accepted (the "genuine" check), and a used-up one is refused.

Cards come from the station's badge list (data/badges.json, role household); filter tags, quotas and the
record from the ledger. No clock and no hardware here: tested on its own (tests/test_water_point.py).
"""

import time
from dataclasses import dataclass
from pathlib import Path

import aeroion_contract as ac

from station.badges import registry

from .quality import Verdict


@dataclass
class PointSettings:
    quota_l: float = 20.0           # a household's daily water, unless its card has its own quota
    idle_s: float = 15.0            # no flow for this long: the valve closes
    max_session_s: float = 600.0    # the longest session
    filter_change_s: float = 30.0   # after a technician's badge: time to tap the new filter's tag
    expiring_fraction: float = 0.9  # the filter is "expiring" past 90 % of its capacity
    min_left_l: float = 0.1         # less quota than this left: the card is refused


def local_day(ms):
    """The station's local day, 'YYYY-MM-DD': quotas start again at local midnight."""
    return time.strftime("%Y-%m-%d", time.localtime(ms / 1000))


def card_name(uid, label=""):
    """What the dashboard and the events show: the card's label, or the end of its UID, never the whole UID."""
    label = (label or "").strip()
    return label[:40] if label else "…" + uid[-4:]


@dataclass
class Session:
    uid: str
    card: str
    started_ms: int
    last_flow_ms: int
    left_at_start: float    # the quota left when it started
    litres: float = 0.0

    @property
    def quota_left(self):
        return max(0.0, self.left_at_start - self.litres)


class WaterPoint:
    def __init__(self, ledger, cards, settings=None, day_of=local_day):
        """cards.lookup(uid) gives (role, label) for a badge of the station's list, or None."""
        self.ledger, self.cards, self.s, self.day_of = ledger, cards, settings or PointSettings(), day_of
        self.quality = Verdict("unknown", "starting: no reading yet")
        self.session = None
        self.filter = ledger.fitted_filter()      # dict (ledger.FILTER_FIELDS), litres kept up to date here
        self.filter_change_until = None           # ms, while waiting for a filter's tag
        self.stray_l = 0.0                        # flow counted with the valve closed (a leak, or drips)
        self._today = (None, 0.0)                 # (day, litres given out in finished sessions)

    @property
    def valve_open(self):
        return self.session is not None

    # ------------------------------------------------------------------ inputs

    def set_quality(self, verdict, now_ms):
        out = []
        if verdict.verdict != self.quality.verdict:
            out.append(self._event("QUALITY_CHANGE", {"verdict": verdict.verdict, "reason": verdict.reason[:80],
                                                      "tds_ppm": verdict.tds_ppm, "ph": verdict.ph}, now_ms))
        self.quality = verdict
        if self.session is not None and verdict.verdict != "ok":
            out += self._end("quality", now_ms)
        return out

    def tap(self, uid, now_ms):
        """A card or tag held to the reader (uid: upper-case hex, as in the contract)."""
        badge = self.cards.lookup(uid)
        if badge is not None:
            role, label = badge
            name = card_name(uid, label)
            if role == "household":
                return self._household(uid, name, now_ms)
            if role in ("technician", "admin"):
                self.filter_change_until = now_ms + self.s.filter_change_s * 1000
                return [("log", f"{name} ({role}): tap the new filter's tag within {self.s.filter_change_s:g} s")]
            return [self._denied(name, f"{role} badge: not a water card", now_ms)]
        tag = self.ledger.filter(uid)
        if tag is not None:
            return self._filter_tag(tag, now_ms)
        return [self._denied(card_name(uid), "unknown card", now_ms)]

    def flow(self, litres, now_ms):
        """Litres counted by the flow meter since the last call."""
        if litres <= 0:
            return []
        if self.session is None:
            self.stray_l += litres
            return []
        s = self.session
        s.litres += litres
        s.last_flow_ms = now_ms
        if self.filter is not None:
            self.filter["litres"] += litres
        if s.quota_left <= 1e-9:
            return self._end("quota", now_ms)
        if self.filter is not None and self.filter["litres"] >= self.filter["capacity_l"]:
            return self._end("filter", now_ms)
        return []

    def tick(self, now_ms):
        """Call often (every 100 ms or so): sessions that ran too long or stopped flowing, the filter-change wait."""
        out = []
        if self.filter_change_until is not None and now_ms > self.filter_change_until:
            self.filter_change_until = None
            out.append(("log", "filter change: no filter tag tapped in time"))
        s = self.session
        if s is not None:
            if now_ms - s.last_flow_ms > self.s.idle_s * 1000:
                out += self._end("idle", now_ms)
            elif now_ms - s.started_ms > self.s.max_session_s * 1000:
                out += self._end("timeout", now_ms)
        return out

    def stop(self, now_ms):
        return self._end("stop", now_ms) if self.session is not None else []

    def reload(self):
        """Between sessions: the admin may have changed the fitted filter from the command line."""
        if self.session is None:
            self.filter = self.ledger.fitted_filter()

    # ------------------------------------------------------------------ inside

    def _household(self, uid, name, now_ms):
        s = self.session
        if s is not None:
            if s.uid == uid:
                return self._end("card", now_ms)
            return [self._denied(name, f"busy: {s.card} is drawing water", now_ms)]
        if self.quality.verdict != "ok":
            what = "water not safe" if self.quality.verdict == "bad" else "water quality unknown"
            return [self._denied(name, f"{what}: {self.quality.reason}"[:80], now_ms)]
        f = self.filter
        if f is None:
            return [self._denied(name, "no filter fitted", now_ms)]
        if f["litres"] >= f["capacity_l"]:
            return [self._denied(name, f"{card_name(f['uid'], f['label'])} used up: change it"[:80], now_ms)]
        quota = self.ledger.quota(uid)
        quota = self.s.quota_l if quota is None else quota
        left = quota - self.ledger.used_on(self.day_of(now_ms), uid)
        if left < self.s.min_left_l:
            return [self._denied(name, f"daily quota used ({quota:g} L)", now_ms)]
        self.session = Session(uid, name, now_ms, now_ms, left)
        return [("log", f"{name}: valve open, {left:.2f} L left today")]

    def _filter_tag(self, tag, now_ms):
        name = card_name(tag["uid"], tag["label"])
        if self.filter_change_until is None:
            return [self._denied(name, "filter tag: a technician's badge first", now_ms)]
        if self.session is not None:
            return [self._denied(name, f"wait: {self.session.card} is drawing water", now_ms)]
        if tag["litres"] >= tag["capacity_l"]:
            return [self._denied(name, f"used up ({tag['litres']:.0f} of {tag['capacity_l']:g} L): not fitted", now_ms)]
        self.filter_change_until = None
        self.ledger.fit_filter(tag["uid"], now_ms)
        self.filter = self.ledger.filter(tag["uid"])
        return [self._event("FILTER_CHANGE", {"filter": name, "capacity_l": tag["capacity_l"],
                                              "litres": round(tag["litres"], 2)}, now_ms)]

    def _end(self, reason, now_ms):
        s, self.session = self.session, None
        f = self.filter
        self.ledger.add_dispense(self.day_of(s.started_ms), s.uid, s.card, s.started_ms, now_ms, s.litres, reason,
                                 f["uid"] if f else None)
        self._today = (None, 0.0)  # counted again from the ledger
        if f is not None:
            self.filter = f = self.ledger.filter(f["uid"]) or f  # the ledger added the session's litres
        out = [self._event("DISPENSE", {"card": s.card, "litres": round(s.litres, 2),
                                        "quota_left_l": round(s.quota_left, 2), "end": reason}, now_ms)]
        if f is not None and f["litres"] >= f["capacity_l"] and f["expired_ms"] is None:
            f["expired_ms"] = now_ms
            self.ledger.set_filter_expired(f["uid"], now_ms)
            out.append(self._event("FILTER_EXPIRED", {"filter": card_name(f["uid"], f["label"]),
                                                      "litres": round(f["litres"], 2),
                                                      "capacity_l": f["capacity_l"]}, now_ms))
        return out

    def _denied(self, name, reason, now_ms):
        return self._event("CARD_DENIED", {"card": name, "reason": reason[:80]}, now_ms)

    def _event(self, etype, data, now_ms):
        """A water_event message, written to the ledger first. Returns ("event", ledger id, message)."""
        msg = {"schema_version": ac.SCHEMA_VERSION, "ts": int(now_ms), "type": etype, "data": data}
        return ("event", self.ledger.add_event(msg, self.day_of(now_ms)), msg)

    # ------------------------------------------------------------------ output

    def filter_status(self):
        f = self.filter
        if f is None:
            return {"tag": None, "state": "none", "litres": None, "capacity_l": None}
        state = ("expired" if f["litres"] >= f["capacity_l"] else
                 "expiring" if f["litres"] >= self.s.expiring_fraction * f["capacity_l"] else "ok")
        return {"tag": card_name(f["uid"], f["label"]), "state": state, "litres": round(f["litres"], 2),
                "capacity_l": f["capacity_l"]}

    def dispensed_today(self, now_ms):
        day = self.day_of(now_ms)
        if self._today[0] != day:
            self._today = (day, self.ledger.total_on(day))
        s = self.session
        return self._today[1] + (s.litres if s is not None and self.day_of(s.started_ms) == day else 0.0)

    def status(self, now_ms):
        """The water message (water.schema.json)."""
        s = self.session
        return {
            "schema_version": ac.SCHEMA_VERSION, "ts": int(now_ms), "quality": self.quality.as_dict(),
            "valve_open": self.valve_open,
            "session": None if s is None else {"card": s.card, "litres": round(s.litres, 2),
                                               "quota_left_l": round(s.quota_left, 2)},
            "filter": self.filter_status(),
            "dispensed_today_l": round(self.dispensed_today(now_ms), 2),
        }


def describe(msg):
    """One line for the terminal, for a water_event message."""
    d, t = msg["data"], msg["type"]
    if t == "DISPENSE":
        return f"{d['card']} drew {d['litres']:.2f} L, {d['quota_left_l']:.2f} L left today (valve closed: {d['end']})"
    if t == "CARD_DENIED":
        return f"{d['card']}: no water ({d['reason']})"
    if t == "QUALITY_CHANGE":
        return f"water quality {d['verdict'].upper()}" + (f": {d['reason']}" if d["reason"] else "")
    if t == "FILTER_CHANGE":
        return f"{d['filter']} fitted ({d['litres']:.0f} of {d['capacity_l']:g} L used)"
    if t == "FILTER_EXPIRED":
        return f"{d['filter']} used up ({d['litres']:.0f} L): no water until it is changed"
    return t


class BadgeBook:
    """The station's badge list (data/badges.json, station.badges), read again whenever the file changes,
    so a card added while the water point runs works at once. A broken file keeps the last good list."""

    def __init__(self, path=registry.DEFAULT_FILE):
        self.path = Path(path)
        self.stamp = ()
        self.by_uid = {}
        self.error = ""

    def refresh(self):
        """Reads the file again if it changed since the last look."""
        try:
            st = self.path.stat()
            stamp = (st.st_mtime_ns, st.st_size)
        except OSError:
            stamp = None
        if stamp == self.stamp:
            return
        self.stamp = stamp
        try:
            badges = registry.load(self.path)["badges"]
        except registry.BadgeError as e:
            self.error = str(e)
            return
        self.error = ""
        self.by_uid = {b["uid"]: b for b in badges}

    def lookup(self, uid):
        self.refresh()
        b = self.by_uid.get(uid)
        return None if b is None else (b["role"], b.get("label", ""))

    def uid_for_label(self, label):
        """The UID of the badge with this label (any case), or None: lets the simulator type 'tap card 7'."""
        self.refresh()
        label = label.strip().lower()
        return next((u for u, b in self.by_uid.items() if b.get("label", "").strip().lower() == label), None)