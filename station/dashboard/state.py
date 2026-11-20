"""Live state of the station for the dashboard (M10).

Built only from contract messages (docs/message-schema.md): the latest values of every node, a short history
for the charts, counters, alerts and an event feed. Plain logic with no network, so it is tested on its own
(tests/test_dashboard_state.py).

Receivers are tolerant (contract rule 8): unknown fields and enum values are ignored, and a missing or
wrong-typed value is shown as unknown. Replayed messages ("buffered": true) go into the history and the
feed at their own time, but never show as live values (rule 5).
"""

import bisect
import math
from collections import deque

import aeroion_contract as ac

# One point of a node's history: [ts, fog_idx_opt, fog_idx_virt, i_ua, mass_g, rate_gpm, hv_level, p_w]
POINT_FIELDS = ("fog_idx_opt", "fog_idx_virt", "i_ua", "mass_g", "rate_gpm", "hv_level", "p_w")
STATE_SEVERITY = {"HARVEST": "good", "BACKOFF": "warning", "SAFE": "critical", "MAINTENANCE": "serious"}
SEVERITY_RANK = {"critical": 0, "serious": 1, "warning": 2, "info": 3, "good": 4}
FEED_SIZE = 200
SEEN_SIZE = 20000     # recent message keys: a message received twice counts once
TEXT_MAX = 120        # node-supplied text (reasons, details) is cut to this length
HEALTH_WARN = 0.5     # electrode health below this: clean it soon
HEALTH_BAD = 0.25     # below this: clean or replace now
BATTERY_LOW_PCT = 20.0
WATER_SILENT_S = 15.0  # the water point publishes every 5 s: three missed messages and it is shown as silent
WATER_VERDICTS = ("ok", "bad", "unknown")
FILTER_STATES = ("ok", "expiring", "expired", "none")

NUMBER_FIELDS = ("hv_level", "fog_idx_opt", "fog_idx_virt", "t_c", "rh_pct", "mass_g", "rate_gpm", "i_ua", "p_w",
                 "e_wh", "arc_risk", "arc_novel", "batt_pct")
FLAG_FIELDS = ("hv_on", "radar_presence", "lid_closed")
WATER_END_WORDS = {"quota": "quota reached", "card": "card tapped again", "idle": "no flow", "timeout": "longest session",
                   "quality": "quality turned bad", "filter": "filter used up", "stop": "water point stopped"}


def number(value):
    """A finite number, or None: a bool, a string or a missing value is unknown."""
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        return None
    return value


def flag(value):
    return value if isinstance(value, bool) else None


def text(value):
    return value[:TEXT_MAX] if isinstance(value, str) else ""


def fmt(value, digits=2):
    value = number(value)
    return "?" if value is None else f"{value:.{digits}f}"


def filter_name(tag):
    """'filter 3' stays as it is; '…7A90' becomes 'filter …7A90'."""
    tag = text(tag)
    return "the filter" if not tag else tag if "filter" in tag.lower() else f"filter {tag}"


def same_major(version):
    return isinstance(version, str) and version.split(".")[0] == ac.SCHEMA_VERSION.split(".")[0]


class Node:
    def __init__(self, node_id):
        self.node_id = node_id
        self.info = {}
        self.status = {}
        self.live = None          # the latest telemetry received live (not replayed)
        self.live_rx_ms = None    # when it arrived, on the station's clock
        self.state_reason = ""    # reason of the last STATE_CHANGE
        self.counters = {"fog_episodes": 0, "arc_warnings": 0, "arcs": 0, "arcs_missed": 0, "faults": 0}
        self.history = []         # points, sorted by ts


class StationState:
    """Everything the dashboard shows. ingest() takes each message; summary() is called every second."""

    def __init__(self, history_s=600, stale_s=5.0):
        self.history_ms = int(history_s * 1000)
        self.stale_ms = stale_s * 1000
        self.nodes = {}
        self.front = None
        self.water = None         # the water point's latest status (M9)
        self.water_rx_ms = None
        self.feed = deque(maxlen=FEED_SIZE)
        self.ignored = 0
        self._seen = set()
        self._seen_order = deque()

    # ------------------------------------------------------------------ input

    def ingest(self, topic, payload, retain=False, rx_ms=0):
        """One message. Returns the updates for the browsers: [(name, data)], name "point", "feed" or "front"
        (the water point's status goes out with the next tick).

        rx_ms is the station's clock when it arrived (it decides whether a node is silent).
        """
        try:
            scope, kind = ac.parse_topic(topic)
        except (ValueError, AttributeError):
            self.ignored += 1
            return []
        if scope == ac.SIM:
            return []  # simulator ground truth: not for the dashboard
        if not isinstance(payload, dict) or not same_major(payload.get("schema_version")):
            self.ignored += 1
            return []
        if scope == ac.STATION:
            if kind == "front":
                return self._front(payload)
            if kind == "water":
                return self._water(payload, retain, rx_ms)
            return self._water_event(payload, rx_ms)
        if kind == "cmd":
            return self._command(scope, payload, rx_ms)
        if payload.get("node_id") != scope:
            self.ignored += 1  # a message on another node's topic
            return []
        node = self.nodes.get(scope) or self.nodes.setdefault(scope, Node(scope))
        if kind == "telemetry":
            return self._telemetry(node, payload, rx_ms)
        if kind == "event":
            return self._event(node, payload, rx_ms)
        if kind == "status":
            node.status = payload
            return []
        if kind == "info":
            node.info = payload
            return []
        return []  # wave: debug and training data

    def _first_time(self, kind, payload):
        """False if this exact message was already received (a replay sent twice, an MQTT redelivery)."""
        key = (payload.get("node_id"), kind, payload.get("seq"), payload.get("ts"),
               payload.get("type") if kind == "water_event" else None)  # water events have no seq
        if key in self._seen:
            return False
        self._seen.add(key)
        self._seen_order.append(key)
        if len(self._seen_order) > SEEN_SIZE:
            self._seen.discard(self._seen_order.popleft())
        return True

    def _telemetry(self, node, t, rx_ms):
        ts = number(t.get("ts"))
        if ts is None or not self._first_time("telemetry", t):
            return []
        buffered = t.get("buffered") is True
        if not buffered:
            node.live = t
            node.live_rx_ms = rx_ms
        point = [int(ts)] + [number(t.get(f)) for f in POINT_FIELDS]
        newest = node.history[-1][0] if node.history else None
        if newest is None or point[0] >= newest:
            node.history.append(point)
        elif point[0] >= newest - self.history_ms:
            node.history.insert(bisect.bisect_right([p[0] for p in node.history], point[0]), point)
        else:
            return []  # older than the charts show
        cut = node.history[-1][0] - self.history_ms
        while node.history and node.history[0][0] < cut:
            node.history.pop(0)
        return [("point", {"node": node.node_id, "p": point, "buffered": buffered})]

    def _event(self, node, e, rx_ms):
        if number(e.get("ts")) is None or not self._first_time("event", e):
            return []
        data = e.get("data") if isinstance(e.get("data"), dict) else {}
        etype = e.get("type")
        c = node.counters
        severity, words = "info", f"event {text(etype)}"
        if etype == "FOG_DETECTED":
            c["fog_episodes"] += 1
            severity, words = "info", f"fog detected ({fmt(data.get('fog_idx'))}, {text(data.get('source'))})"
        elif etype == "FOG_CLEARED":
            words = f"fog cleared after {fmt(data.get('duration_s'), 0)} s"
        elif etype == "ARC_PREDICTED":
            c["arc_warnings"] += 1
            severity = "warning"
            words = f"arc predicted (risk {fmt(data.get('risk'))} at HV {fmt(data.get('hv_level'))}): backing off"
        elif etype == "ARC":
            c["arcs"] += 1
            predicted = data.get("predicted") is True
            if not predicted:
                c["arcs_missed"] += 1
            severity = "warning" if predicted else "serious"
            words = (f"arc, peak {fmt(data.get('peak_ua'), 0)} µA at HV {fmt(data.get('hv_level'))} "
                     f"({'predicted' if predicted else 'NOT predicted'})")
        elif etype == "STATE_CHANGE":
            to = text(data.get("to"))
            reason = text(data.get("reason"))
            if not e.get("buffered"):
                node.state_reason = reason
            severity = STATE_SEVERITY.get(to, "info")
            words = f"{text(data.get('from'))} → {to}" + (f" ({reason})" if reason else "")
        elif etype == "BADGE_TAP":
            granted = data.get("result") == "granted"
            severity = "info" if granted else "warning"
            action = text(data.get("action"))
            words = (f"badge {text(data.get('uid'))} ({text(data.get('role'))}): "
                     f"{action if action and action != 'none' else 'tap'} {'granted' if granted else 'DENIED'}")
        elif etype == "FAULT":
            c["faults"] += 1
            severity = "critical"
            words = f"fault {text(data.get('code'))}: {text(data.get('detail'))}"
        elif etype == "MAINT_ON":
            severity, words = "serious", f"maintenance lock-out by badge {text(data.get('uid'))}"
        elif etype == "MAINT_OFF":
            words = f"maintenance released by badge {text(data.get('uid'))}"
        elif etype == "CMD_ACK":
            result = text(data.get("result"))
            severity = "info" if result == "accepted" else "warning"
            reason = text(data.get("reason"))
            words = f"command {text(data.get('cmd_id'))} {result}" + (f": {reason}" if reason else "")
        return self._feed(node.node_id, int(e["ts"]), rx_ms, severity, words, e.get("buffered") is True)

    def _command(self, scope, cmd, rx_ms):
        """Commands sent by any station tool (this dashboard included): shown so the CMD_ACK has its context."""
        ts = number(cmd.get("ts"))
        params = cmd.get("params") if isinstance(cmd.get("params"), dict) else {}
        ctype = text(cmd.get("type"))
        detail = {"LOCK": text(params.get("reason")), "SET_HINT": f"HV {fmt(params.get('hv_level'))}",
                  "PREARM": f"fog from {text(params.get('source_node'))} in {fmt(params.get('eta_s'), 0)} s"
                  }.get(ctype, "")
        words = f"command {ctype} {text(cmd.get('cmd_id'))} sent" + (f" ({detail})" if detail else "")
        return self._feed("all" if scope == ac.BROADCAST else scope, int(ts or 0), rx_ms, "info", words, False)

    def _front(self, f):
        if not isinstance(f.get("active"), bool):
            self.ignored += 1
            return []
        eta = f.get("eta_s") if isinstance(f.get("eta_s"), dict) else {}
        self.front = {
            "ts": number(f.get("ts")), "active": f["active"], "direction_deg": number(f.get("direction_deg")),
            "speed_mps": number(f.get("speed_mps")), "fit_quality": number(f.get("fit_quality")),
            "nodes_detected": [n for n in f.get("nodes_detected") or [] if ac.is_valid_node_id(n)],
            "eta_s": {n: number(s) for n, s in eta.items() if ac.is_valid_node_id(n) and number(s) is not None},
        }
        return [("front", self.front)]

    def _water(self, w, retain, rx_ms):
        q = w.get("quality") if isinstance(w.get("quality"), dict) else {}
        session = w.get("session") if isinstance(w.get("session"), dict) else None
        f = w.get("filter") if isinstance(w.get("filter"), dict) else {}
        if not isinstance(w.get("valve_open"), bool):
            self.ignored += 1
            return []
        ts = number(w.get("ts"))
        self.water = {
            "ts": ts, "valve_open": w["valve_open"], "dispensed_today_l": number(w.get("dispensed_today_l")),
            "quality": {"verdict": q.get("verdict") if q.get("verdict") in WATER_VERDICTS else "unknown",
                        "reason": text(q.get("reason")), "tds_ppm": number(q.get("tds_ppm")),
                        "ph": number(q.get("ph")), "temp_c": number(q.get("temp_c"))},
            "session": None if session is None else {
                "card": text(session.get("card")), "litres": number(session.get("litres")),
                "quota_left_l": number(session.get("quota_left_l"))},
            "filter": {"tag": text(f.get("tag")) or None,
                       "state": f.get("state") if f.get("state") in FILTER_STATES else None,
                       "litres": number(f.get("litres")), "capacity_l": number(f.get("capacity_l"))},
        }
        # A retained status is as old as its time says (the water point runs on the station's clock)
        self.water_rx_ms = min(rx_ms, int(ts)) if retain and ts is not None else rx_ms
        return []

    def _water_event(self, e, rx_ms):
        if number(e.get("ts")) is None or not self._first_time("water_event", e):
            return []
        data = e.get("data") if isinstance(e.get("data"), dict) else {}
        etype = e.get("type")
        severity, words = "info", f"water point: {text(etype)}"
        if etype == "DISPENSE":
            end = text(data.get("end"))
            words = (f"{text(data.get('card'))} drew {fmt(data.get('litres'), 1)} L, "
                     f"{fmt(data.get('quota_left_l'), 1)} L left today"
                     + (f" (valve closed: {WATER_END_WORDS.get(end, end)})" if end else ""))
            severity = "warning" if end in ("quality", "filter") else "info"
        elif etype == "CARD_DENIED":
            severity = "warning"
            words = f"{text(data.get('card'))}: no water ({text(data.get('reason'))})"
        elif etype == "QUALITY_CHANGE":
            verdict = text(data.get("verdict"))
            severity = {"ok": "good", "bad": "serious"}.get(verdict, "warning")
            reason = text(data.get("reason"))
            words = f"water quality {verdict.upper() or '?'}" + (f": {reason}" if reason else "")
        elif etype == "FILTER_CHANGE":
            words = (f"{filter_name(data.get('filter'))} fitted ({fmt(data.get('litres'), 0)} of "
                     f"{fmt(data.get('capacity_l'), 0)} L used)")
        elif etype == "FILTER_EXPIRED":
            severity = "serious"
            words = f"{filter_name(data.get('filter'))} used up ({fmt(data.get('litres'), 0)} L): no water until it is changed"
        return self._feed("water", int(e["ts"]), rx_ms, severity, words, e.get("buffered") is True)

    def _feed(self, node_id, ts, rx_ms, severity, words, buffered):
        entry = {"ts": ts, "rx_ms": rx_ms, "node": node_id, "severity": severity, "text": words,
                 "buffered": buffered}
        self.feed.append(entry)
        return [("feed", entry)]

    # ------------------------------------------------------------------ output

    def node_summary(self, node, now_ms):
        t = node.live or {}
        s = node.status
        health = t.get("health") if isinstance(t.get("health"), dict) else {}
        position = node.info.get("position") if isinstance(node.info.get("position"), dict) else {}
        online = flag(s.get("online"))
        age_s = None if node.live_rx_ms is None else max(0.0, (now_ms - node.live_rx_ms) / 1000)
        out = {
            "node_id": node.node_id,
            "online": online,
            "age_s": None if age_s is None else round(age_s, 1),
            "live": node.live is not None and online is not False and age_s * 1000 <= self.stale_ms,
            "ts": number(t.get("ts")),
            "state": t.get("state") if t.get("state") in ac.STATES else None,
            "state_reason": node.state_reason,
            "health": {k: number(health.get(k)) for k in ("onset_level", "k_coef", "score")},
            "time_synced": flag(s.get("time_synced")),
            "rssi_dbm": number(s.get("rssi_dbm")),
            "buffered_msgs": number(s.get("buffered_msgs")),
            "uptime_s": number(s.get("uptime_s")),
            "fw": text(node.info.get("fw")),
            "sensors": [text(x) for x in node.info.get("sensors") or [] if isinstance(x, str)],
            "position": ({"x_m": number(position.get("x_m")), "y_m": number(position.get("y_m"))}
                         if number(position.get("x_m")) is not None and number(position.get("y_m")) is not None
                         else None),
            "counters": dict(node.counters),
        }
        out.update({f: number(t.get(f)) for f in NUMBER_FIELDS})
        out.update({f: flag(t.get(f)) for f in FLAG_FIELDS})
        out["alerts"] = self.node_alerts(out)
        return out

    def node_alerts(self, n):
        """What needs attention now, worst first: [{"severity", "text"}]."""
        alerts = []

        def add(severity, words):
            alerts.append({"severity": severity, "text": words})

        if n["online"] is False:
            add("critical", "offline (the broker lost the node)")
        elif n["age_s"] is None:
            add("warning", "no telemetry yet")
        elif not n["live"]:
            add("warning", f"no telemetry for {n['age_s']:.0f} s")
        if n["state"] == "SAFE":
            add("critical", "SAFE: HV forced off" + (f" ({n['state_reason']})" if n["state_reason"] else ""))
        elif n["state"] == "MAINTENANCE":
            add("serious", "maintenance lock-out: only the technician's badge releases it")
        if n["radar_presence"] is True:
            add("warning", "person near the collector: HV cut by the radar")
        if n["lid_closed"] is False:
            add("warning", "lid open")
        score = n["health"]["score"]
        if score is not None and score < HEALTH_BAD:
            add("serious", f"electrode health {score:.2f}: clean or replace now")
        elif score is not None and score < HEALTH_WARN:
            add("warning", f"electrode health {score:.2f}: clean it soon")
        if n["time_synced"] is False:
            add("warning", "clock not synced: ignored by the fog-front tracker")
        if n["batt_pct"] is not None and n["batt_pct"] < BATTERY_LOW_PCT:
            add("warning", f"battery {n['batt_pct']:.0f} %")
        if n["buffered_msgs"]:
            add("info", f"{n['buffered_msgs']:.0f} message(s) waiting in the node's offline buffer")
        alerts.sort(key=lambda a: SEVERITY_RANK[a["severity"]])
        return alerts

    def water_summary(self, now_ms):
        """The water point (M9) with its age, or None if it was never heard."""
        if self.water is None:
            return None
        age_s = max(0.0, (now_ms - self.water_rx_ms) / 1000)
        return dict(self.water, age_s=round(age_s, 1), live=age_s <= WATER_SILENT_S)

    def water_alerts(self, w):
        alerts = []

        def add(severity, words):
            alerts.append({"severity": severity, "text": words, "node": "water"})

        if w is None:
            return alerts
        q, f = w["quality"], w["filter"]
        if not w["live"]:
            add("warning", f"water point silent for {w['age_s']:.0f} s"
                + (f" (last status: {q['reason']})" if q["reason"] else ""))
            return alerts  # the rest is out of date
        if q["verdict"] == "bad":
            add("serious", f"water not safe ({q['reason'] or 'quality bad'}): the valve stays closed")
        elif q["verdict"] == "unknown":
            add("warning", f"water quality unknown ({q['reason'] or 'no reading'}): the valve stays closed")
        name = filter_name(f["tag"])
        used = (f" ({f['litres']:.0f} of {f['capacity_l']:.0f} L)"
                if f["litres"] is not None and f["capacity_l"] is not None else "")
        if f["state"] == "expired":
            add("serious", f"{name} used up{used}: change it, no water until then")
        elif f["state"] == "expiring":
            add("warning", f"{name} nearly used up{used}: change it soon")
        elif f["state"] == "none":
            add("warning", "no filter fitted: a technician taps their badge, then the filter's tag")
        return alerts

    def summary(self, now_ms):
        """Every node, the station totals and the alerts: sent to the browsers every second."""
        nodes = [self.node_summary(self.nodes[k], now_ms) for k in sorted(self.nodes)]
        water_point = self.water_summary(now_ms)

        def total(field):
            values = [n[field] for n in nodes if n[field] is not None]
            return sum(values) if values else None

        def rounded(value):
            return None if value is None else round(value, 3)

        water, energy = total("mass_g"), total("e_wh")
        counters = {k: sum(n["counters"][k] for n in nodes) for k in ("arc_warnings", "arcs", "arcs_missed",
                                                                       "fog_episodes", "faults")}
        totals = {
            "water_g": rounded(water), "rate_gpm": rounded(total("rate_gpm")), "energy_wh": rounded(energy),
            "power_w": rounded(total("p_w")),
            "g_per_wh": round(water / energy, 1) if water is not None and energy else None,
            "nodes": len(nodes), "nodes_live": sum(1 for n in nodes if n["live"]),
            "nodes_harvesting": sum(1 for n in nodes if n["live"] and n["state"] == "HARVEST"),
            **counters,
        }
        alerts = [dict(a, node=n["node_id"]) for n in nodes for a in n["alerts"]] + self.water_alerts(water_point)
        alerts.sort(key=lambda a: SEVERITY_RANK[a["severity"]])
        return {"now_ms": now_ms, "nodes": nodes, "totals": totals, "alerts": alerts, "water": water_point}

    def snapshot(self, now_ms):
        """Everything, for a browser that just connected."""
        out = self.summary(now_ms)
        out["history"] = {k: list(n.history) for k, n in sorted(self.nodes.items())}
        out["history_s"] = self.history_ms // 1000
        out["fields"] = list(POINT_FIELDS)
        out["front"] = self.front
        out["feed"] = list(self.feed)
        return out