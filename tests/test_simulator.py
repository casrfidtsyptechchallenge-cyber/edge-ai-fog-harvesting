"""Tests for the M0 simulator and the contract checker."""

import io
import json
import subprocess
import sys
from collections import defaultdict

import pytest

import aeroion_contract as ac
from conftest import REPO
from contract_check import Checker
from fake_node import CollectSink, Simulation

T0 = 1791747040.0


def run_sim(seconds, **kwargs):
    sink = CollectSink()
    sim = Simulation(sink, t0=T0, **kwargs)
    sim.start()
    for _ in range(int(seconds)):
        sim.step()
    return sim, sink


def events(sink, etype=None, node=None):
    out = []
    for topic, payload, _ in sink.messages:
        if topic.endswith("/event") and (etype is None or payload["type"] == etype) \
                and (node is None or payload["node_id"] == node):
            out.append(payload)
    return out


@pytest.fixture(scope="module")
def long_run():
    return run_sim(3600, nodes=3, seed=11, wave=True, truth=True, battery=True)


def test_every_message_is_valid_and_seq_is_continuous(long_run):
    _, sink = long_run
    checker = Checker(quiet=True, check_clock=False, out=io.StringIO())
    for topic, payload, _ in sink.messages:
        assert checker.check(topic, payload), (topic, payload)
    assert sum(checker.invalid.values()) == 0
    assert not checker.warnings, checker.warnings


def test_birth_messages_are_retained(long_run):
    _, sink = long_run
    first = sink.messages[:6]
    assert {t.rsplit("/", 1)[1] for t, _, _ in first} == {"info", "status"}
    assert all(retain for _, _, retain in first)


def test_nodes_get_armed_and_harvest(long_run):
    _, sink = long_run
    for nid in ("n01", "n02", "n03"):
        targets = [e["data"]["to"] for e in events(sink, "STATE_CHANGE", nid)]
        assert "ARMED" in targets and "HARVEST" in targets
        assert events(sink, "BADGE_TAP", nid)


def test_fog_reaches_nodes_in_the_order_the_front_predicts(long_run):
    sim, sink = long_run
    truths = [p for t, p, _ in sink.messages if t == "aeroion/sim/truth"]
    assert truths, "no fog episode in an hour"
    first = truths[0]
    expected = sorted(first["arrival_ts"], key=first["arrival_ts"].get)
    detections = {}
    for e in events(sink, "FOG_DETECTED"):
        detections.setdefault(e["node_id"], e["ts"])
    seen = sorted(detections, key=detections.get)
    # Arrival times closer than the fog ramp can swap, so only demand that the first and last agree.
    assert seen[0] == expected[0] and seen[-1] == expected[-1]


def test_water_and_energy_accumulate(long_run):
    _, sink = long_run
    last = {}
    for topic, payload, _ in sink.messages:
        if topic.endswith("/telemetry"):
            last[payload["node_id"]] = payload
    for t in last.values():
        assert t["mass_g"] > 1.0 and t["e_wh"] > 0.0
        assert t["batt_pct"] < 100


def test_arc_predictor_acts(long_run):
    _, sink = long_run
    predicted = events(sink, "ARC_PREDICTED")
    assert predicted
    backoffs = [e for e in events(sink, "STATE_CHANGE") if e["data"]["to"] == "BACKOFF"]
    assert len(backoffs) >= len(predicted)
    waves = [p for t, p, _ in sink.messages if t.endswith("/wave")]
    assert waves and all(len(w["samples"]) == 256 for w in waves)


def test_the_novelty_guard_shows_on_the_interfered_node(long_run):
    _, sink = long_run
    novel = defaultdict(list)
    for topic, payload, _ in sink.messages:
        if topic.endswith("/telemetry") and payload.get("arc_novel") is not None:
            novel[payload["node_id"]].append(payload["arc_novel"])
    assert max(novel["n01"]) > 0.3                       # interference episodes on n01
    assert max(novel["n02"] + novel["n03"]) < 0.05       # the others stay normal
    waves = [p for t, p, _ in sink.messages if t.endswith("/wave") and p["trigger"] == "NOVEL"]
    assert waves and {w["node_id"] for w in waves} == {"n01"}


def test_fouled_electrode_shows_in_health_score(long_run):
    _, sink = long_run
    scores = defaultdict(list)
    for topic, payload, _ in sink.messages:
        if topic.endswith("/telemetry") and payload["health"]["score"] is not None:
            scores[payload["node_id"]].append(payload["health"]["score"])
    assert scores["n03"][-1] < scores["n01"][-1] - 0.1


def test_virtual_fog_sensor_tracks_optical_sensor(long_run):
    _, sink = long_run
    pairs = [(p["fog_idx_opt"], p["fog_idx_virt"]) for t, p, _ in sink.messages
             if t.endswith("/telemetry") and p["fog_idx_virt"] is not None]
    assert len(pairs) > 100
    mean_abs_err = sum(abs(a - b) for a, b in pairs) / len(pairs)
    assert mean_abs_err < 0.15


# ---------------------------------------------------------------- commands

def cmd(ctype, params, ts_s, ttl_s=30, cmd_id="c-1"):
    return {"schema_version": ac.SCHEMA_VERSION, "cmd_id": cmd_id, "ts": int(ts_s * 1000), "ttl_s": ttl_s,
            "type": ctype, "params": params}


def last_ack(sink):
    return events(sink, "CMD_ACK")[-1]["data"]


def test_lock_command_locks_and_acks():
    sim, sink = run_sim(30, nodes=1, seed=3)
    assert sim.nodes["n01"].state != "LOCKED"
    sim.send_command("n01", cmd("LOCK", {"reason": "test"}, sim.t))
    assert sim.nodes["n01"].state == "LOCKED"
    assert last_ack(sink) == {"cmd_id": "c-1", "result": "accepted", "reason": ""}


def test_broadcast_lock_reaches_every_node():
    sim, sink = run_sim(30, nodes=3, seed=3)
    sim.send_command("all", cmd("LOCK", {"reason": "test"}, sim.t))
    assert all(n.state == "LOCKED" for n in sim.nodes.values())
    assert len(events(sink, "CMD_ACK")) == 3


def test_expired_command_is_ignored():
    sim, sink = run_sim(30, nodes=1, seed=3)
    state = sim.nodes["n01"].state
    sim.send_command("n01", cmd("LOCK", {"reason": "late"}, sim.t - 120, ttl_s=10))
    assert sim.nodes["n01"].state == state
    assert last_ack(sink)["result"] == "expired"


def test_invalid_command_is_rejected_with_reason():
    sim, sink = run_sim(30, nodes=1, seed=3)
    sim.send_command("n01", cmd("PREARM", {"hv_level": 0.5}, sim.t))
    ack = last_ack(sink)
    assert ack["result"] == "rejected" and ack["reason"].startswith("invalid")


def test_prearm_needs_armed_state():
    sim, sink = run_sim(1, nodes=1, seed=3)
    assert sim.nodes["n01"].state == "LOCKED"
    sim.send_command("n01", cmd("PREARM", {"eta_s": 10, "source_node": "n02"}, sim.t))
    assert last_ack(sink)["result"] == "rejected"


def test_tare_zeroes_the_scale():
    sim, sink = run_sim(1200, nodes=1, seed=5)
    assert sim.nodes["n01"].mass > 0
    sim.send_command("n01", cmd("TARE", {}, sim.t))
    assert sim.nodes["n01"].mass == 0
    assert last_ack(sink)["result"] == "accepted"


def test_stop_publishes_offline_status():
    sim, sink = run_sim(5, nodes=2, seed=1)
    sim.stop()
    tail = [(t, p) for t, p, _ in sink.messages[-2:]]
    assert all(t.endswith("/status") and p["online"] is False for t, p in tail)


# ---------------------------------------------------------------- command line

def test_cli_stdout_pipes_into_checker():
    sim = subprocess.run([sys.executable, str(REPO / "sim" / "fake_node.py"), "--stdout", "--no-wait",
                          "--duration", "300", "--seed", "2", "--truth"],
                         capture_output=True, text=True, check=True)
    lines = sim.stdout.splitlines()
    assert len(lines) > 900
    check = subprocess.run([sys.executable, str(REPO / "tools" / "contract_check.py"), "--stdin", "--no-clock",
                            "--quiet"], input=sim.stdout, capture_output=True, text=True)
    assert check.returncode == 0, check.stdout
    assert " 0 invalid" in check.stdout


def test_checker_flags_bad_lines():
    bad = json.dumps({"topic": "aeroion/n01/status", "payload": {"node_id": "n01"}}) + "\nnot json\n"
    check = subprocess.run([sys.executable, str(REPO / "tools" / "contract_check.py"), "--stdin", "--no-clock"],
                           input=bad, capture_output=True, text=True)
    assert check.returncode == 1
    assert "ERROR" in check.stdout


def test_cli_out_file_then_check_file(tmp_path):
    """The Windows-friendly way: no pipes, no redirection."""
    out = tmp_path / "run.jsonl"
    subprocess.run([sys.executable, str(REPO / "sim" / "fake_node.py"), "--out", str(out), "--no-wait",
                    "--duration", "120", "--seed", "4"], capture_output=True, text=True, check=True)
    assert out.read_bytes().isascii()
    check = subprocess.run([sys.executable, str(REPO / "tools" / "contract_check.py"), "--no-clock", "--quiet",
                            str(out)], capture_output=True, text=True)
    assert check.returncode == 0, check.stdout
    assert " 0 invalid" in check.stdout


@pytest.mark.parametrize("encoding", ["utf-16", "utf-8-sig", "utf-8"])
def test_checker_reads_files_in_any_encoding(tmp_path, encoding):
    """Windows PowerShell's > writes UTF-16 with a BOM; Notepad may add a UTF-8 BOM."""
    sim, sink = run_sim(30, nodes=1, seed=1)
    text = "\r\n".join(json.dumps({"topic": t, "payload": p}) for t, p, _ in sink.messages) + "\r\n"
    path = tmp_path / "run.jsonl"
    path.write_text(text, encoding=encoding)
    check = subprocess.run([sys.executable, str(REPO / "tools" / "contract_check.py"), "--no-clock", "--quiet",
                            str(path)], capture_output=True, text=True)
    assert check.returncode == 0, check.stdout
    assert f"{len(sink.messages)} message(s) checked, 0 invalid" in check.stdout
