"""Contract messages in, contract messages out (M8): joins the tracker to the broker's topics.

In:  aeroion/<node>/info (position), status (clock synced, online), telemetry (state), event (FOG_DETECTED,
     FOG_CLEARED, and the CMD_ACK answers to our PREARMs).
Out: aeroion/station/front (retained) and PREARM commands on aeroion/<node>/cmd.

No network and no clock here: the caller gives the time (the station's clock live, the messages' time when
scoring a recorded run). Receivers are tolerant (contract rule 8): anything unexpected is ignored.
"""

import math
import uuid
from collections import deque

import aeroion_contract as ac

PREARM_TTL_S = 30


def _number(v):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) else None


class NetworkEngine:
    def __init__(self, tracker, ttl_s=PREARM_TTL_S):
        self.tracker = tracker
        self.ttl_s = ttl_s
        self.sent = {}                   # cmd_id -> node, for the CMD_ACK answers
        self.acks = {"accepted": 0, "rejected": 0, "expired": 0}
        self._seen = set()
        self._seen_order = deque()

    def _first_time(self, payload):
        key = (payload.get("node_id"), payload.get("seq"), payload.get("ts"))
        if key in self._seen:
            return False
        self._seen.add(key)
        self._seen_order.append(key)
        if len(self._seen_order) > 5000:
            self._seen.discard(self._seen_order.popleft())
        return True

    def handle(self, topic, payload, now_ms):
        """One message. Returns actions: ("publish", topic, payload, retain), ("log", text), ("learned", prior)."""
        try:
            scope, kind = ac.parse_topic(topic)
        except (ValueError, AttributeError):
            return []
        if not ac.is_valid_node_id(scope) or not isinstance(payload, dict) or payload.get("node_id") != scope:
            return []
        if not str(payload.get("schema_version", "")).startswith(ac.SCHEMA_VERSION.split(".")[0] + "."):
            return []
        t = self.tracker
        if kind == "info":
            pos = payload.get("position") if isinstance(payload.get("position"), dict) else {}
            x, y = _number(pos.get("x_m")), _number(pos.get("y_m"))
            if x is not None and y is not None:
                t.set_position(scope, x, y)
            return []
        if kind == "status":
            if isinstance(payload.get("online"), bool):
                t.set_online(scope, payload["online"])
            if isinstance(payload.get("time_synced"), bool):
                t.set_synced(scope, payload["time_synced"])
            return []
        if kind == "telemetry":
            if payload.get("buffered") is not True and payload.get("state") in ac.STATES:
                t.set_state(scope, payload["state"])
            return []
        if kind != "event" or _number(payload.get("ts")) is None or not self._first_time(payload):
            return []
        etype = payload.get("type")
        data = payload.get("data") if isinstance(payload.get("data"), dict) else {}
        if etype == "FOG_DETECTED":
            return self._actions(t.detected(scope, int(payload["ts"]), now_ms), now_ms)
        if etype == "FOG_CLEARED":
            return self._actions(t.cleared(scope, int(payload["ts"]), now_ms), now_ms)
        if etype == "CMD_ACK" and data.get("cmd_id") in self.sent:
            result = data.get("result")
            if result in self.acks:
                self.acks[result] += 1
            reason = data.get("reason") if isinstance(data.get("reason"), str) else ""
            return [("log", f"PREARM to {scope}: {result}" + (f" ({reason[:80]})" if reason else ""))]
        return []

    def tick(self, now_ms):
        return self._actions(self.tracker.tick(now_ms), now_ms)

    def _actions(self, outputs, now_ms):
        actions = []
        for out in outputs:
            if out[0] == "front":
                actions.append(("publish", ac.station_topic("front"), out[1], True))
            elif out[0] == "prearm":
                _, node, eta, source = out
                cmd = {"schema_version": ac.SCHEMA_VERSION, "cmd_id": f"m8-{uuid.uuid4().hex[:8]}",
                       "ts": int(now_ms), "ttl_s": self.ttl_s, "type": "PREARM",
                       "params": {"eta_s": eta, "source_node": source}}
                self.sent[cmd["cmd_id"]] = node
                if len(self.sent) > 1000:
                    self.sent.pop(next(iter(self.sent)))
                actions.append(("publish", ac.node_topic(node, "cmd"), cmd, False))
                actions.append(("log", f"PREARM to {node}: fog expected in {eta:.0f} s (seen first at {source})"))
            else:
                actions.append(out)
        return actions