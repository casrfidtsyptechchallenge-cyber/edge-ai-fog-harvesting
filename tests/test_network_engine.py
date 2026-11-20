"""Tests for station/network engine, score and service (M8): contract messages in and out, scoring on the
simulator, the learned usual front kept in a file."""

import json
import subprocess
import sys
from pathlib import Path

import pytest

import aeroion_contract as ac
from station.network import service
from station.network.engine import NetworkEngine
from station.network.score import score_run
from station.network.tracker import FrontTracker, Prior

REPO = Path(__file__).resolve().parents[1]
T0 = 1791747040000


def info(node, x, y):
    return {"schema_version": "1.1", "node_id": node, "ts": T0, "fw": "t", "position": {"x_m": x, "y_m": y},
            "models": {"arc": None, "fog": None, "health": None}, "sensors": []}


def event(node, seq, etype, data, ts=T0, **extra):
    return {"schema_version": "1.1", "node_id": node, "ts": ts, "seq": seq, "type": etype, "data": data, **extra}


def telemetry(node, state, **extra):
    t = json.loads((REPO / "contract" / "examples" / "telemetry.harvest.json").read_text(encoding="utf-8"))
    t.update(node_id=node, state=state, **extra)
    return t


@pytest.fixture
def engine():
    e = NetworkEngine(FrontTracker())
    for node, (x, y) in {"n01": (0, 0), "n02": (15, 12), "n03": (30, 0)}.items():
        e.handle(f"aeroion/{node}/info", info(node, x, y), T0)
        e.handle(f"aeroion/{node}/status", {"schema_version": "1.1", "node_id": node, "online": True,
                                             "time_synced": True}, T0)
        e.handle(f"aeroion/{node}/telemetry", telemetry(node, "ARMED"), T0)
    return e


def test_a_detection_becomes_a_front_message_and_prearm_commands(engine):
    actions = engine.handle("aeroion/n01/event", event("n01", 5, "FOG_DETECTED", {"fog_idx": 0.4, "source": "optical"}), T0)
    published = [a for a in actions if a[0] == "publish"]
    topics = [a[1] for a in published]
    assert topics == ["aeroion/n02/cmd", "aeroion/n03/cmd", "aeroion/station/front"]
    for _, topic, payload, retain in published:
        kind = ac.parse_topic(topic)[1]
        assert ac.validate(kind, payload) == [], payload
        assert retain is (kind == "front")
    cmd = published[0][2]
    assert cmd["type"] == "PREARM" and cmd["params"]["source_node"] == "n01" and cmd["ttl_s"] == 30
    ack = event("n02", 9, "CMD_ACK", {"cmd_id": cmd["cmd_id"], "result": "rejected", "reason": "state is SAFE"})
    assert engine.handle("aeroion/n02/event", ack, T0 + 100) == [("log", "PREARM to n02: rejected (state is SAFE)")]
    assert engine.acks["rejected"] == 1


def test_what_the_engine_ignores(engine):
    detection = event("n01", 5, "FOG_DETECTED", {"fog_idx": 0.4, "source": "optical"})
    assert engine.handle("aeroion/n01/event", detection, T0)
    assert engine.handle("aeroion/n01/event", detection, T0) == []                      # the same message twice
    assert engine.handle("aeroion/n02/event", detection, T0) == []                      # on another node's topic
    assert engine.handle("aeroion/n02/event", dict(detection, node_id="n02", schema_version="2.0"), T0) == []
    assert engine.handle("aeroion/station/front", {"active": True}, T0) == []          # our own output
    assert engine.handle("not/a/topic", detection, T0) == []
    engine.handle("aeroion/n03/telemetry", telemetry("n03", "SAFE", buffered=True), T0)  # replayed: not the state now
    assert engine.tracker.places["n03"].state == "ARMED"
    engine.handle("aeroion/n03/status", {"schema_version": "1.1", "node_id": "n03", "online": False}, T0)
    assert engine.tracker.places["n03"].state is None                                  # offline: no PREARM


def test_cleared_fog_and_the_end_of_a_front(engine):
    for i, (node, dt) in enumerate((("n01", 0), ("n02", 10_000), ("n03", 20_000))):
        engine.handle(f"aeroion/{node}/event", event(node, 10 + i, "FOG_DETECTED", {"fog_idx": 0.4, "source": "optical"},
                                                     ts=T0 + dt), T0 + dt)
    for i, node in enumerate(("n01", "n02", "n03")):
        engine.handle(f"aeroion/{node}/event", event(node, 20 + i, "FOG_CLEARED", {"duration_s": 120.0}, ts=T0 + 150_000),
                      T0 + 150_000)
    actions = engine.tick(T0 + 300_000)
    front = [a[2] for a in actions if a[0] == "publish"][0]
    assert front["active"] is False and ac.validate("front", front) == []
    assert [a[0] for a in actions][-1] == "learned"


def simulate(tmp_path, *args):
    run = tmp_path / "run.jsonl"
    subprocess.run([sys.executable, str(REPO / "sim" / "fake_node.py"), "--out", str(run), "--no-wait",
                    "--truth", *args], check=True, capture_output=True)
    return run


def test_the_tracker_on_a_simulator_run(tmp_path):
    report = score_run(simulate(tmp_path, "--nodes", "5", "--duration", "3600", "--seed", "2"))
    assert report["fronts_true"] >= 5 and report["fronts_tracked"] == report["fronts_true"]
    assert report["fitted"] == report["fronts_tracked"]
    assert report["median_dir_err_deg"] < 5 and report["median_speed_err_pct"] < 10
    assert report["prearmed_in_time"] == report["later_nodes"] > 0  # every ARMED node reached later
    assert report["median_lead_s"] > 5 and report["median_eta_err_s"] < 5


def test_a_prevailing_wind_is_learned(tmp_path):
    report = score_run(simulate(tmp_path, "--duration", "7200", "--seed", "3", "--wind", "90"))
    p = report["prior"]
    assert p.fronts >= 5 and p.agreement > 0.8 and abs(p.direction_deg - 90) < 15
    assert report["predictions"] > 0 and report["median_eta_err_s"] < 10
    assert report["prearmed_in_time"] == report["later_nodes"]


def test_the_usual_front_is_kept_in_a_file(tmp_path):
    path = tmp_path / "data" / "network_prior.json"
    p = Prior.toward(90, 1.5, source="learned", fronts=4)
    service.save_prior(path, p)
    assert service.load_prior(path) == p
    path.write_text("{broken", encoding="utf-8")
    assert service.load_prior(path) is None
    assert service.load_prior(tmp_path / "missing.json") is None


class FakeClient:
    def __init__(self):
        self.published = []

    def publish(self, topic, payload, qos=0, retain=False):
        self.published.append((topic, json.loads(payload), retain))


def test_the_service_publishes_and_remembers(tmp_path):
    logs = []
    svc = service.NetworkService(NetworkEngine(FrontTracker()), prior_file=tmp_path / "p.json", log=logs.append,
                                 clock=lambda: T0)
    svc.client = FakeClient()
    cmd = {"schema_version": "1.1", "cmd_id": "m8-1", "ts": T0, "ttl_s": 30, "type": "PREARM",
           "params": {"eta_s": 10.0, "source_node": "n01"}}
    svc.do([("publish", "aeroion/n02/cmd", cmd, False), ("log", "hello"),
            ("learned", Prior.toward(45, 1.0, source="learned", fronts=1))])
    assert svc.client.published == [("aeroion/n02/cmd", cmd, False)]
    assert logs[0] == "hello" and "toward 45°" in logs[1]
    assert service.load_prior(tmp_path / "p.json").fronts == 1
    nothing = service.no_front(T0)
    assert ac.validate("front", nothing) == [] and nothing["active"] is False