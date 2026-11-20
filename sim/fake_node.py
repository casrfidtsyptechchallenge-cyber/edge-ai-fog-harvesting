#!/usr/bin/env python3
"""Fake AeroIon-Edge collector nodes (module M0 simulator).

Publishes contract-valid messages for N nodes so the station side (M8, M9, M10) can be built and tested
before any hardware exists. Fog fronts cross the field in random directions, every node follows the M3
state machine, and commands from the station are obeyed and acknowledged with CMD_ACK.

    # JSON lines into a file, simulated clock, no broker needed (datasets, tests, tools):
    python sim/fake_node.py --out run.jsonl --no-wait --duration 1800

    # Same, but printed to the terminal (Linux/macOS pipes):
    python sim/fake_node.py --stdout --no-wait --duration 1800

    # Live on MQTT in real time; --compress 4 makes fog episodes happen 4x faster:
    python sim/fake_node.py --broker localhost --nodes 3 --compress 4

The physics is a plausible toy model, not measured data: it exists to exercise the software. Replace it
with logged runs from the real chamber as soon as M2 produces them.

With --truth, the true front direction and speed are published on aeroion/sim/truth so the fog-front
tracker (M8) can be scored. That topic is simulator-only and not part of the contract.
"""

import argparse
import json
import math
import queue
import random
import sys
import time
from functools import partial
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "contract"))
import aeroion_contract as ac  # noqa: E402

DT = 1.0                 # simulation step, s
FOG_ON = 0.30            # fog index that triggers FOG_DETECTED
FOG_ON_PREARMED = 0.20   # lower threshold while pre-armed by the station
FOG_OFF = 0.12           # fog index that triggers FOG_CLEARED (hysteresis)
PRESENCE_CLEAR_S = 10.0   # after a person, the area must be clear this long before re-arming (safety core)
ARC_WARN = 0.70          # arc_risk that triggers ARC_PREDICTED and BACKOFF
NOVEL_HOLD = 0.20        # arc_novel above this: the HV controller (M6) stops climbing, as on the node
BACKOFF_S = 4.0
ENERGY_PENALTY = 0.15   # g/min per W, toy controller objective
FW_VERSION = "sim-1.0"


def clamp(x, lo=0.0, hi=1.0):
    return max(lo, min(hi, x))


def default_positions(n):
    """Nodes 15 m apart in x, alternating between y = 0 and y = 12 m, so any three neighbours form a triangle.

    The fog-front tracker needs non-collinear nodes: collinear nodes only reveal the speed along their line.
    """
    return [(15.0 * i, 12.0 if i % 2 else 0.0) for i in range(n)]


# ---------------------------------------------------------------- fog world

class FogFront:
    """One fog episode: a straight front crossing the field at constant speed."""

    def __init__(self, start, direction_deg, speed_mps, duration_s, peak, ramp_s, positions):
        self.start = start
        self.direction_deg = direction_deg
        self.speed_mps = speed_mps
        self.duration_s = duration_s
        self.peak = peak
        self.ramp_s = ramp_s
        # The front appears 10 m before the first node it will reach.
        self.s_entry = min(self._along(x, y) for x, y in positions) - 10.0
        self.end = max(self.arrival(x, y) for x, y in positions) + duration_s

    def _along(self, x, y):
        th = math.radians(self.direction_deg)
        return x * math.sin(th) + y * math.cos(th)

    def arrival(self, x, y):
        return self.start + (self._along(x, y) - self.s_entry) / self.speed_mps

    def density(self, t, x, y):
        a = self.arrival(x, y)
        if t < a:
            return 0.0
        rise = min(1.0, (t - a) / self.ramp_s)
        fall = clamp((a + self.duration_s - t) / self.ramp_s)
        return self.peak * rise * fall


class FogWorld:
    """Generates fog episodes one after another, with random gaps of clear air in between."""

    def __init__(self, rng, positions, t0, mean_gap_s=240.0, compress=1.0, wind_deg=None):
        self.rng = rng
        self.positions = positions
        self.wind_deg = wind_deg  # None: fronts come from any direction; else they move toward it, ±20°
        self.mean_gap_s = mean_gap_s / compress
        self.compress = compress
        self.front = None
        self.next_start = t0 + rng.uniform(15, 30) / compress   # first fog arrives soon

    def update(self, t):
        """Advance the world; return the new FogFront if one starts at this step."""
        if self.front is not None and t > self.front.end:
            self.front = None
            self.next_start = t + max(30.0 / self.compress, self.rng.expovariate(1.0 / self.mean_gap_s))
        if self.front is None and t >= self.next_start:
            c = self.compress
            direction = (self.rng.uniform(0, 360) if self.wind_deg is None
                         else (self.rng.gauss(self.wind_deg, 20.0)) % 360.0)
            self.front = FogFront(start=t, direction_deg=direction,
                                  speed_mps=self.rng.uniform(0.5, 2.0) * c,
                                  duration_s=self.rng.uniform(120, 300) / c,
                                  peak=self.rng.uniform(0.55, 0.95),
                                  ramp_s=15.0 / c, positions=self.positions)
            return self.front
        
        return None

    def density(self, t, x, y):
        return self.front.density(t, x, y) if self.front else 0.0


# ---------------------------------------------------------------- one node

class SimNode:
    """A collector node: sensors, M3 state machine, a toy HV controller and toy physics."""

    def __init__(self, node_id, position, rng, t0, emit, wave=False, battery=False, fouling_per_s=0.0,
                 compress=1.0, novelty=False):
        self.node_id = node_id
        self.position = position
        self.rng = rng
        self.emit = emit                      # emit(kind, payload, retain=False)
        self.wave = wave
        self.compress = compress
        self.started = t0
        self.seq = 0

        self.state = "LOCKED"
        self.safe_cause = None
        self.badge_at = t0 + rng.uniform(3, 8)            # the operator arms the node shortly after boot
        self.operator_uid = f"04{rng.randrange(16 ** 6):06X}"
        self.allowlist_version = 0

        self.hv_level = 0.0
        self.level_target = 0.5
        self.hint = None                                   # (hv_level, valid_until)
        self.prearm_until = None
        self.backoff_until = None
        self.last_prediction_t = -math.inf
        self.es_next = t0 + 8.0                            # extremum-seeking controller
        self.es_last_eff = None
        self.es_dir = 1.0

        self.presence_until = -math.inf
        self.next_presence = t0 + rng.uniform(150, 400) / compress

        self.fog_flag = False
        self.fog_since = None

        self.mass = 0.0
        self.e_wh = 0.0
        self.rate_ema = 0.0
        self.onset = 0.22 + rng.uniform(-0.02, 0.02)       # true corona onset (hv_level units)
        self.onset0 = self.onset
        self.k = 160.0 + rng.uniform(-15, 15)              # true Townsend coefficient
        self.fouling_per_s = fouling_per_s                 # how fast the electrode gets dirty
        self.onset_est = None
        self.k_est = None
        self.batt = 100.0 if battery else None

        # M5a novelty guard stand-in: on one node, now and then, interference makes the arc predictor's windows
        # unlike its training data (arc_novel ~0.5 for a minute or two). Its own random stream: the other
        # nodes' runs stay exactly as before.
        self.novelty = novelty
        self.nrng = random.Random(f"novelty/{node_id}")
        self.novel_share = 0.0
        self.novel_until = -math.inf
        self.novel_next = t0 + self.nrng.uniform(240, 480) / compress
        self.novel_wave_t = -math.inf

    # ------------------------------------------------ helpers

    def _next_seq(self):
        self.seq += 1
        return self.seq

    def _envelope(self, t, with_seq=True):
        msg = {"schema_version": ac.SCHEMA_VERSION, "node_id": self.node_id, "ts": int(t * 1000)}
        if with_seq:
            msg["seq"] = self._next_seq()
        return msg

    def _event(self, t, etype, data):
        self.emit("event", {**self._envelope(t), "type": etype, "data": data})

    def _set_state(self, t, new, reason):
        if new == self.state:
            return
        old, self.state = self.state, new
        self._event(t, "STATE_CHANGE", {"from": old, "to": new, "reason": reason})

    def _ack(self, t, cmd_id, result, reason=""):
        self._event(t, "CMD_ACK", {"cmd_id": cmd_id, "result": result, "reason": reason[:64]})

    # ------------------------------------------------ birth and heartbeat

    def info(self, t):
        return {**self._envelope(t, with_seq=False), "fw": FW_VERSION,
                "models": {"arc": "sim", "fog": "sim", "health": "sim"},
                "position": {"x_m": self.position[0], "y_m": self.position[1]},
                "sensors": ["SIM-OPTICS", "SIM-SHT45", "SIM-HX711", "SIM-INA219", "SIM-LD2410"]}

    def status(self, t, online=True):
        msg = {**self._envelope(t, with_seq=False), "online": online}
        if online:
            msg.update({"uptime_s": int(t - self.started), "time_synced": True, "buffered_msgs": 0,
                        "rssi_dbm": round(-55 + self.rng.gauss(0, 3), 1)})
        return msg

    # ------------------------------------------------ commands from the station

    def handle_command(self, t, raw):
        """Apply one command (bytes, str or dict) and answer with CMD_ACK."""
        try:
            cmd = json.loads(raw) if isinstance(raw, (bytes, str)) else raw
        except (ValueError, UnicodeDecodeError):
            print(f"[{self.node_id}] ignored a command that is not JSON", file=sys.stderr)
            return
        cmd_id = cmd.get("cmd_id") if isinstance(cmd, dict) else None
        errors = ac.validate("cmd", cmd)
        if errors:
            if isinstance(cmd_id, str) and cmd_id:
                self._ack(t, cmd_id[:36], "rejected", "invalid: " + errors[0])
            else:
                print(f"[{self.node_id}] ignored an invalid command without cmd_id: {errors}", file=sys.stderr)
            return
        if t * 1000 > cmd["ts"] + cmd["ttl_s"] * 1000:
            self._ack(t, cmd_id, "expired", "ts + ttl_s is in the past")
            return

        ctype, params = cmd["type"], cmd["params"]
        if ctype == "LOCK":
            self.hv_level = 0.0
            self._set_state(t, "LOCKED", "station LOCK: " + params["reason"])
            self.badge_at = t + self.rng.uniform(20, 40) / self.compress   # sim only: operator re-arms later
            self._ack(t, cmd_id, "accepted")
        elif ctype == "PREARM":
            if self.state != "ARMED":
                self._ack(t, cmd_id, "rejected", f"state is {self.state}, PREARM needs ARMED")
            else:
                self.prearm_until = t + params["eta_s"] + 60.0
                self._ack(t, cmd_id, "accepted")
        elif ctype == "SET_HINT":
            self.hint = (params["hv_level"], cmd["ts"] / 1000 + cmd["ttl_s"])
            self._ack(t, cmd_id, "accepted")
        elif ctype == "SYNC_ALLOWLIST":
            if params["version"] < self.allowlist_version:
                self._ack(t, cmd_id, "rejected", "older allowlist version")
            else:
                self.allowlist_version = params["version"]
                self._ack(t, cmd_id, "accepted")
        elif ctype == "TARE":
            self.mass = 0.0
            self._ack(t, cmd_id, "accepted")

    # ------------------------------------------------ one simulation step

    def step(self, t, dt, fog_true):
        rng = self.rng

        # Sensors
        fog_opt = clamp(fog_true + rng.gauss(0, 0.02))
        t_c = 14.0 + 2.0 * math.sin(2 * math.pi * (t - self.started) / 3600) - 1.5 * fog_true + rng.gauss(0, 0.1)
        rh = clamp(72 + 28 * fog_true + rng.gauss(0, 0.8), 0, 100)

        # Fog detection with hysteresis
        prearmed = self.prearm_until is not None and t < self.prearm_until
        threshold = FOG_ON_PREARMED if prearmed else FOG_ON
        if not self.fog_flag and fog_opt > threshold:
            self.fog_flag, self.fog_since = True, t
            self._event(t, "FOG_DETECTED", {"fog_idx": round(fog_opt, 3), "source": "optical"})
        elif self.fog_flag and fog_opt < FOG_OFF:
            self.fog_flag = False
            self._event(t, "FOG_CLEARED", {"duration_s": round(t - self.fog_since, 1)})

        # Radar: someone walks up to the node from time to time
        if t >= self.next_presence:
            self.presence_until = t + rng.uniform(5, 12)
            self.next_presence = t + rng.uniform(200, 500) / self.compress
        presence = t < self.presence_until

        # M3 state machine (see docs/message-schema.md, "Node state machine")
        if presence and self.state in ("ARMED", "HARVEST", "BACKOFF"):
            self.safe_cause = "presence"
            self._set_state(t, "SAFE", "person detected by radar")
        elif self.state == "SAFE" and self.safe_cause == "presence" and not presence:
            # as the safety core: re-arm only after PRESENCE_CLEAR_S with nobody seen
            if t - self.presence_until >= PRESENCE_CLEAR_S:
                self.safe_cause = None
                self._set_state(t, "ARMED", "area clear")
        elif self.state == "LOCKED" and self.badge_at is not None and t >= self.badge_at:
            self.badge_at = None
            self._event(t, "BADGE_TAP", {"uid": self.operator_uid, "role": "operator", "action": "arm",
                                         "result": "granted"})
            self._set_state(t, "ARMED", "operator badge")
        elif self.state == "ARMED" and self.fog_flag:
            self._set_state(t, "HARVEST", "fog detected")
        elif self.state == "HARVEST" and not self.fog_flag:
            self._set_state(t, "ARMED", "fog cleared")
        elif self.state == "BACKOFF" and t >= self.backoff_until:
            self._set_state(t, "HARVEST" if self.fog_flag else "ARMED", "backoff over")

        # HV controller (M6 stand-in): follow the station hint, else extremum-seek the objective
        # "collection rate minus an energy penalty". Its optimum sits close to the arc threshold, so the
        # arc predictor gets to act, as it will on the real bench.
        hv_on = self.state in ("HARVEST", "BACKOFF")
        lo = self.onset + 0.08
        if self.novelty and t >= self.novel_next:
            self.novel_until = t + self.nrng.uniform(60, 120) / self.compress
            self.novel_next = self.novel_until + self.nrng.uniform(300, 600) / self.compress
        target = 0.5 if t < self.novel_until else 0.01
        self.novel_share += (target - self.novel_share) * min(1.0, dt * self.compress / 5.0)  # 50 windows = 5 s
        if self.state == "HARVEST":
            if self.hint is not None and t <= self.hint[1]:
                self.level_target = clamp(self.hint[0], lo, 0.95)
            elif t >= self.es_next:
                p_now = 0.4 + 1.6 * self.hv_level ** 2
                eff = self.rate_ema - ENERGY_PENALTY * p_now
                if self.es_last_eff is not None and eff < self.es_last_eff:
                    self.es_dir = -self.es_dir
                self.es_last_eff = eff
                before = self.level_target
                self.level_target = clamp(self.level_target + 0.04 * self.es_dir, lo, 0.95)
                if self.novel_share > NOVEL_HOLD:              # the predictor is unsure: do not climb
                    self.level_target = min(self.level_target, before)
                self.es_next = t + 8.0
            step_max = 0.05 * dt
            self.hv_level = clamp(self.hv_level + clamp(self.level_target - self.hv_level, -step_max, step_max))
        elif self.state == "BACKOFF":
            self.hv_level = max(lo, self.hv_level - 0.1 * dt)
        else:
            self.hv_level = 0.0
        v = self.hv_level

        # Toy physics: corona current (Townsend, lowered by fog), capture, power, energy
        self.onset += self.fouling_per_s * dt
        if hv_on and v > self.onset:
            i_clean = self.k * v * (v - self.onset)
            i_ua = max(0.0, i_clean * (1 - 0.45 * fog_true) * (1 + rng.gauss(0, 0.02)))
            capture = 0.85 * (1 - math.exp(-6 * (v - self.onset)))
            self.onset_est = self.onset + rng.gauss(0, 0.004)          # M5c Townsend fit stand-in
            self.k_est = self.k * (1 + rng.gauss(0, 0.01))
        else:
            i_ua = max(0.0, rng.gauss(0.2, 0.1))
            capture = 0.0
        rate_true = 1.2 * fog_true * (0.03 + capture)                  # g/min; 0.03 = passive capture
        self.mass += rate_true * dt / 60
        self.rate_ema = max(0.0, 0.85 * self.rate_ema + 0.15 * (rate_true + rng.gauss(0, 0.01)))
        p_w = 0.4 + 1.6 * v ** 2 if hv_on else 0.0
        self.e_wh += p_w * dt / 3600
        if self.batt is not None:
            self.batt = clamp(self.batt - (p_w + 0.5) * dt / 3600 / 30 * 100, 0, 100)

        # M5b virtual fog sensor: how much fog suppresses the current, given the estimated clean current
        fog_virt = None
        if hv_on and self.onset_est is not None and v > self.onset_est + 0.05:
            i_clean_est = self.k_est * v * (v - self.onset_est)
            if i_clean_est > 1.0:
                fog_virt = round(clamp((1 - i_ua / i_clean_est) / 0.45), 3)

        # M5a arc predictor stand-in: risk rises steeply near a fog-dependent threshold
        arc_risk = None
        if hv_on:
            arc_risk = 1 / (1 + math.exp(-30 * (v - (0.82 - 0.12 * fog_true))))
            if self.state == "HARVEST" and arc_risk > ARC_WARN:
                self.last_prediction_t = t
                self._event(t, "ARC_PREDICTED", {"risk": round(arc_risk, 3), "hv_level": round(v, 3)})
                self._wave(t, "ARC_PREDICTED", i_ua)
                self.backoff_until = t + BACKOFF_S
                self.level_target = max(lo, v - 0.12)
                self.es_last_eff = None
                self._set_state(t, "BACKOFF", "arc predicted")
            elif arc_risk > 0.3 and rng.random() < 0.02 * arc_risk * dt:
                self._event(t, "ARC", {"peak_ua": round(i_ua * rng.uniform(20, 60), 1), "hv_level": round(v, 3),
                                       "predicted": t - self.last_prediction_t < 5})
                self._wave(t, "ARC", i_ua)
                self.backoff_until = t + BACKOFF_S
                self._set_state(t, "BACKOFF", "arc")

        if hv_on and self.novel_share > NOVEL_HOLD and t - self.novel_wave_t >= 10.0 / self.compress:
            self.novel_wave_t = t
            self._wave(t, "NOVEL", i_ua)                        # training material for M11

        health_score = None
        if self.onset_est is not None:
            health_score = round(clamp(1 - (self.onset_est - self.onset0) / 0.15), 3)

        self.emit("telemetry", {
            **self._envelope(t),
            "state": self.state,
            "hv_on": self.state in ("HARVEST", "BACKOFF"),
            "hv_level": round(self.hv_level, 3),
            "fog_idx_opt": round(fog_opt, 3),
            "fog_idx_virt": fog_virt,
            "t_c": round(t_c, 2),
            "rh_pct": round(rh, 1),
            "mass_g": round(max(0.0, self.mass + rng.gauss(0, 0.02)), 2),
            "rate_gpm": round(self.rate_ema, 3),
            "i_ua": round(i_ua, 2),
            "p_w": round(p_w, 3),
            "e_wh": round(self.e_wh, 4),
            "arc_risk": None if arc_risk is None else round(arc_risk, 3),
            "arc_novel": round(self.novel_share, 2) if hv_on else None,
            "health": {
                "onset_level": None if self.onset_est is None else round(clamp(self.onset_est), 4),
                "k_coef": None if self.k_est is None else round(self.k_est, 1),
                "score": health_score,
            },
            "radar_presence": presence,
            "lid_closed": True,
            "batt_pct": None if self.batt is None else round(self.batt, 1),
        })

    def _wave(self, t, trigger, i_ua):
        """Synthetic current window. Toy data for building the pipeline, not for training the real model."""
        if not self.wave:
            return
        fs, n = 10_000, 256
        rng = self.rng
        samples = []
        for k in range(n):
            x = i_ua + rng.gauss(0, max(0.5, 0.03 * i_ua))
            if trigger == "ARC_PREDICTED" and rng.random() < 0.02 + 0.08 * k / n:
                x += rng.uniform(2, 6) * (1 + 3 * k / n)      # growing pre-discharge pulses
            if trigger == "ARC" and k >= 200:
                x += i_ua * 40 * math.exp(-(k - 200) / 8)     # the arc itself
            samples.append(round(max(0.0, x), 2))
        self.emit("wave", {**self._envelope(t - n / fs), "trigger": trigger, "fs_hz": fs, "unit": "uA",
                           "samples": samples})


# ---------------------------------------------------------------- output sinks

class StdoutSink:
    """One JSON line per message: {"topic": ..., "retain": ..., "payload": {...}}.

    Lines are pure ASCII (other characters are escaped as \\uXXXX), so any terminal can print them.
    """

    def __init__(self, stream=None):
        self.stream = stream or sys.stdout

    def publish(self, topic, payload, retain=False):
        self.stream.write(json.dumps({"topic": topic, "retain": retain, "payload": payload}) + "\n")
        self.stream.flush()

    def poll_commands(self):
        return []

    def close(self):
        pass


class FileSink(StdoutSink):
    """Same JSON lines, written to a UTF-8 file. Avoids shell redirection, which differs between systems
    (Windows PowerShell's > writes UTF-16)."""

    def __init__(self, path):
        super().__init__(open(path, "w", encoding="utf-8", newline="\n"))

    def close(self):
        self.stream.close()


class CollectSink(StdoutSink):
    """Keeps messages in memory (used by the tests)."""

    def __init__(self):
        self.messages = []
        self.inbox = []

    def publish(self, topic, payload, retain=False):
        self.messages.append((topic, payload, retain))

    def poll_commands(self):
        out, self.inbox = self.inbox, []
        return out


class MqttSink:
    """One MQTT client per simulated node, each with its own last will, like the real ESP32s."""

    def __init__(self, host, port, node_ids, keepalive=30):
        import paho.mqtt.client as mqtt

        self.inbox = queue.Queue()
        self.clients = {}
        self.last_info = {}
        for nid in node_ids:
            if hasattr(mqtt, "CallbackAPIVersion"):           # paho-mqtt 2.x
                c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"aeroion-sim-{nid}")
            else:                                             # paho-mqtt 1.x
                c = mqtt.Client(client_id=f"aeroion-sim-{nid}")
            lwt = {"schema_version": ac.SCHEMA_VERSION, "node_id": nid, "online": False}
            c.will_set(ac.node_topic(nid, "status"), json.dumps(lwt), qos=1, retain=True)
            c.on_connect = partial(self._on_connect, nid)
            c.on_message = partial(self._on_message, nid)
            c.connect(host, port, keepalive)
            c.loop_start()
            self.clients[nid] = c

    @staticmethod
    def _on_connect(nid, client, *args):
        client.subscribe([(ac.node_topic(nid, "cmd"), 1), (ac.node_topic(ac.BROADCAST, "cmd"), 1)])

    def _on_message(self, nid, client, userdata, msg):
        self.inbox.put((nid, msg.topic, msg.payload))

    def publish(self, topic, payload, retain=False):
        scope, kind = ac.parse_topic(topic)
        client = self.clients.get(scope) or next(iter(self.clients.values()))
        qos = 0 if kind in ("telemetry", "wave") else 1
        self.last_info[client] = client.publish(topic, json.dumps(payload, ensure_ascii=False), qos=qos,
                                                retain=retain)

    def poll_commands(self):
        out = []
        while not self.inbox.empty():
            out.append(self.inbox.get_nowait())
        return out

    def close(self):
        for c in self.clients.values():
            info = self.last_info.get(c)
            if info is not None:
                try:
                    info.wait_for_publish(timeout=2)
                except (RuntimeError, ValueError):
                    pass                                   # not connected: nothing more to flush
            c.disconnect()
            c.loop_stop()


# ---------------------------------------------------------------- runner

class Simulation:
    def __init__(self, sink, nodes=3, seed=None, t0=None, wave=False, battery=False, truth=False,
                 fog_gap_s=240.0, compress=1.0, positions=None, wind_deg=None):
        self.sink = sink
        self.rng = random.Random(seed)
        self.t = time.time() if t0 is None else t0
        self.truth = truth
        positions = positions or default_positions(nodes)
        self.world = FogWorld(self.rng, positions, self.t, mean_gap_s=fog_gap_s, compress=compress, wind_deg=wind_deg)
        self.nodes = {}
        for i, pos in enumerate(positions):
            nid = ac.node_id_from_number(i + 1)
            # The last node's electrode fouls quickly, so the health score has something to show.
            fouling = 2e-5 if (i == len(positions) - 1 and len(positions) > 1) else 0.0
            # The first node gets interference now and then, so the novelty guard (arc_novel) has something to show.
            self.nodes[nid] = SimNode(nid, pos, random.Random(self.rng.random()), self.t,
                                      partial(self._emit, nid), wave=wave, battery=battery,
                                      fouling_per_s=fouling * compress, compress=compress,
                                      novelty=(i == 0 and len(positions) > 1))

    def _emit(self, nid, kind, payload, retain=False):
        errors = ac.validate(kind, payload)
        if errors:
            raise RuntimeError(f"simulator produced an invalid {kind} for {nid}: {errors}")
        self.sink.publish(ac.node_topic(nid, kind), payload, retain=retain)

    def start(self):
        for nid, node in self.nodes.items():
            self._emit(nid, "info", node.info(self.t), retain=True)
            self._emit(nid, "status", node.status(self.t), retain=True)

    def send_command(self, node_id, cmd):
        """Deliver a command as if it came from the broker (tests and scripted demos)."""
        targets = self.nodes.values() if node_id == ac.BROADCAST else [self.nodes[node_id]]
        for node in targets:
            node.handle_command(self.t, cmd)

    def step(self):
        t = self.t
        front = self.world.update(t)
        if front is not None and self.truth:
            arrivals = {nid: int(front.arrival(*n.position) * 1000) for nid, n in self.nodes.items()}
            self.sink.publish(f"{ac.TOPIC_ROOT}/{ac.SIM}/truth", {
                "ts": int(t * 1000), "direction_deg": round(front.direction_deg, 1),
                "speed_mps": round(front.speed_mps, 3), "peak": round(front.peak, 3),
                "duration_s": round(front.duration_s, 1), "arrival_ts": arrivals})
        for nid, _topic, raw in self.sink.poll_commands():
            self.nodes[nid].handle_command(t, raw)
        for node in self.nodes.values():
            node.step(t, DT, self.world.density(t, *node.position))
            if int(t - node.started) % 10 == 0:
                self._emit(node.node_id, "status", node.status(t), retain=True)
        self.t += DT

    def stop(self):
        for nid, node in self.nodes.items():
            self._emit(nid, "status", node.status(self.t, online=False), retain=True)
        self.sink.close()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--nodes", type=int, default=3, help="number of nodes (1..99), default 3")
    p.add_argument("--out", metavar="FILE", help="write JSON lines to FILE instead of publishing to MQTT")
    p.add_argument("--stdout", action="store_true", help="print JSON lines instead of publishing to MQTT")
    p.add_argument("--no-wait", action="store_true",
                   help="run as fast as possible on a simulated clock (only with --out or --stdout)")
    p.add_argument("--duration", type=float, default=None,
                   help="stop after this many simulated seconds (default: run until Ctrl-C)")
    p.add_argument("--broker", default="localhost", help="MQTT broker host, default localhost")
    p.add_argument("--port", type=int, default=1883, help="MQTT broker port, default 1883")
    p.add_argument("--compress", type=float, default=1.0,
                   help="make fog episodes happen N times faster (for live development), default 1")
    p.add_argument("--fog-gap", type=float, default=240.0, help="mean clear-air gap between fog episodes, s")
    p.add_argument("--seed", type=int, default=None, help="random seed, for repeatable runs")
    p.add_argument("--wave", action="store_true", help="also publish synthetic current windows")
    p.add_argument("--battery", action="store_true", help="simulate a battery instead of bench power")
    p.add_argument("--truth", action="store_true", help="publish true front parameters on aeroion/sim/truth")
    p.add_argument("--wind", type=float, default=None, metavar="DEG",
                   help="prevailing wind: fronts move toward DEG (0 = north, 90 = east) +-20 deg; default: any direction")
    args = p.parse_args(argv)

    if not 1 <= args.nodes <= 99:
        p.error("--nodes must be between 1 and 99")
    if args.out and args.stdout:
        p.error("choose either --out or --stdout")
    if args.no_wait and not (args.stdout or args.out):
        p.error("--no-wait only works with --out or --stdout (MQTT runs in real time)")
    if args.compress <= 0:
        p.error("--compress must be positive")
    if args.no_wait and args.duration is None:
        args.duration = 1800.0

    node_ids = [ac.node_id_from_number(i + 1) for i in range(args.nodes)]
    if args.out:
        try:
            sink = FileSink(args.out)
        except OSError as e:
            print(f"Cannot write {args.out}: {e}", file=sys.stderr)
            return 2
        print(f"Writing {' '.join(node_ids)} to {args.out} ...", file=sys.stderr)
    elif args.stdout:
        sink = StdoutSink()
    else:
        try:
            sink = MqttSink(args.broker, args.port, node_ids)
        except OSError as e:
            print(f"Cannot reach the MQTT broker at {args.broker}:{args.port} ({e}).\n"
                  "Is Mosquitto running? To try without a broker, use --out run.jsonl.", file=sys.stderr)
            return 2
        print(f"Simulating {' '.join(node_ids)} on mqtt://{args.broker}:{args.port}. Ctrl-C to stop.",
              file=sys.stderr)

    sim = Simulation(sink, nodes=args.nodes, seed=args.seed, wave=args.wave, battery=args.battery,
                     truth=args.truth, fog_gap_s=args.fog_gap, compress=args.compress, wind_deg=args.wind)
    start_t = sim.t
    sim.start()
    try:
        while args.duration is None or sim.t - start_t < args.duration:
            sim.step()
            if not args.no_wait:
                time.sleep(max(0.0, sim.t - time.time()))
    except KeyboardInterrupt:
        pass
    finally:
        sim.stop()
    if args.out:
        print(f"Done: {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
