"""Tests for M2 (station/datalog): reading, labelling, dataset tables, command line, recorder."""

import csv
import json
import os
import socket
import subprocess
import sys

import pytest

import aeroion_contract as ac
from conftest import REPO
from fake_node import CollectSink, Simulation
from station.datalog import dataset, labels, reader, recorder, runs

T0 = 1791747040000  # ms


# ------------------------------------------------------------------------------------------ helpers

def telemetry(seq, ts, **fields):
    """A valid telemetry payload; fields override the defaults."""
    msg = {"schema_version": ac.SCHEMA_VERSION, "node_id": "n01", "ts": ts, "seq": seq, "state": "HARVEST",
           "hv_on": True, "hv_level": 0.6, "fog_idx_opt": 0.7, "fog_idx_virt": None, "t_c": 14.0,
           "rh_pct": 98.0, "mass_g": 0.0, "rate_gpm": 0.0, "i_ua": 20.0, "p_w": 1.0, "e_wh": 0.0,
           "arc_risk": 0.1, "health": {"onset_level": None, "k_coef": None, "score": None},
           "radar_presence": False, "lid_closed": True, "batt_pct": None}
    msg.update(fields)
    return msg


def event(seq, ts, etype, data):
    return {"schema_version": ac.SCHEMA_VERSION, "node_id": "n01", "ts": ts, "seq": seq, "type": etype,
            "data": data}


def line(kind, payload, node="n01"):
    return json.dumps({"topic": f"aeroion/{node}/{kind}", "payload": payload})


def make_run(tmp_path, lines, tag="t"):
    run_dir, _ = runs.new_run(tmp_path, tag)
    (run_dir / runs.RAW_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return run_dir


def read_csv(path):
    with open(path, encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def flat_rows(messages):
    return [dataset.flatten(m) for m in messages]


# ------------------------------------------------------------------------------------------ runs

def test_settings_are_parsed_with_types():
    assert runs.parse_settings(["gap_mm=15", "level=0.6", "hv_mode=ai", "fan=true"]) == {
        "gap_mm": 15, "level": 0.6, "hv_mode": "ai", "fan": True}
    with pytest.raises(ValueError):
        runs.parse_settings(["gap_mm"])


def test_new_run_creates_folder_and_meta(tmp_path):
    run_dir, meta = runs.new_run(tmp_path, "Gap 15 mm!", {"gap_mm": 15})
    assert meta["run_id"].endswith("_gap-15-mm")
    assert runs.read_meta(run_dir)["settings"] == {"gap_mm": 15}
    assert runs.resolve_run("latest", tmp_path) == run_dir
    assert runs.resolve_run(meta["run_id"], tmp_path) == run_dir


# ------------------------------------------------------------------------------------------ reader

def test_reader_keeps_valid_messages_and_reports_the_rest(tmp_path):
    good = telemetry(1, T0)
    bad = telemetry(2, T0 + 1000, fog_idx_opt=1.7)  # out of range
    run_dir = make_run(tmp_path, [
        line("telemetry", good),
        line("telemetry", good),  # exact duplicate
        line("telemetry", bad),
        "not json at all",
        json.dumps({"topic": "aeroion/sim/truth", "payload": {}}),  # skipped, not an error
    ])
    msgs = reader.read_run(run_dir / runs.RAW_FILE)
    assert len(msgs.telemetry["n01"]) == 1
    assert msgs.stats["duplicates"] == 1
    assert msgs.stats["invalid"] == 1
    assert msgs.stats["unreadable"] == 1
    assert msgs.stats["skipped"] == 1
    assert len(msgs.problems) == 2


def test_reader_sorts_by_time_and_counts_lost_messages(tmp_path):
    run_dir = make_run(tmp_path, [line("telemetry", telemetry(s, T0 + s * 1000)) for s in (3, 1, 2, 6)])
    msgs = reader.read_run(run_dir / runs.RAW_FILE)
    assert [m["seq"] for m in msgs.telemetry["n01"]] == [1, 2, 3, 6]
    assert msgs.quality["n01"] == {"lost": 2, "restarts": 0}  # 4 and 5 never arrived


def test_reader_detects_a_node_restart(tmp_path):
    seqs = list(range(1, 101)) + list(range(1, 11))  # rebooted after message 100
    run_dir = make_run(tmp_path, [line("telemetry", telemetry(s, T0 + i * 1000)) for i, s in enumerate(seqs)])
    msgs = reader.read_run(run_dir / runs.RAW_FILE)
    assert msgs.quality["n01"] == {"lost": 0, "restarts": 1}
    assert len(msgs.telemetry["n01"]) == 110  # same seq after a reboot is not a duplicate


def test_reader_accepts_utf16_files(tmp_path):
    run_dir, _ = runs.new_run(tmp_path, "u16")
    (run_dir / runs.RAW_FILE).write_text(line("telemetry", telemetry(1, T0)) + "\n", encoding="utf-16")
    assert reader.read_run(run_dir / runs.RAW_FILE).stats["kept"] == 1


# ------------------------------------------------------------------------------------------ columns

def test_columns_follow_the_contract():
    columns = dataset.telemetry_columns()
    assert columns[:4] == ["schema_version", "node_id", "ts", "seq"]
    assert {"health_onset_level", "health_k_coef", "health_score"} <= set(columns)
    assert "health" not in columns


def test_a_new_contract_field_appears_by_itself():
    schema = json.loads(json.dumps(ac.SCHEMAS["telemetry"]))  # deep copy
    schema["properties"]["wind_mps"] = {"type": ["number", "null"]}
    assert dataset.telemetry_columns(schema)[-1] == "wind_mps"


# ------------------------------------------------------------------------------------------ labels

def test_rate_label_finds_a_steady_rate():
    rows = flat_rows([telemetry(s, T0 + s * 1000, mass_g=0.5 * s / 60) for s in range(120)])  # 0.5 g/min
    labels.apply_labels(rows, [], labels.load_config())
    middle = [r["label_rate_gpm"] for r in rows[20:100]]
    assert all(abs(v - 0.5) < 1e-6 for v in middle)


def test_rate_label_ignores_a_tare():
    masses = [30 + s / 60 for s in range(60)] + [s / 60 for s in range(60)]  # 1 g/min, bottle emptied at 60 s
    rows = flat_rows([telemetry(i, T0 + i * 1000, mass_g=m) for i, m in enumerate(masses)])
    labels.apply_labels(rows, [], labels.load_config())
    rates = [r["label_rate_gpm"] for r in rows if r["label_rate_gpm"] is not None]
    assert all(abs(v - 1.0) < 1e-6 for v in rates)  # no huge negative rate around the reset


def test_rate_label_needs_enough_samples():
    rows = flat_rows([telemetry(s, T0 + s * 1000, mass_g=s / 60) for s in range(5)])
    labels.apply_labels(rows, [], labels.load_config())
    assert all(r["label_rate_gpm"] is None for r in rows)


def test_arc_label_marks_the_seconds_before_an_arc():
    rows = flat_rows([telemetry(s, T0 + s * 1000) for s in range(20)])
    labels.apply_labels(rows, [T0 + 10_000], labels.load_config())
    marked = [i for i, r in enumerate(rows) if r["label_arc_next"] == 1]
    assert marked == [5, 6, 7, 8, 9]  # arc at 10 s, horizon 5 s


def test_fog_class_and_config_override(tmp_path):
    rows = flat_rows([telemetry(1, T0, fog_idx_opt=0.1), telemetry(2, T0 + 1000, fog_idx_opt=0.3),
                      telemetry(3, T0 + 2000, fog_idx_opt=0.9), telemetry(4, T0 + 3000, fog_idx_opt=None)])
    labels.apply_labels(rows, [], labels.load_config())
    assert [r["label_fog_class"] for r in rows] == ["none", "light", "dense", None]

    custom = tmp_path / "label_config.json"
    custom.write_text(json.dumps({"fog_thresholds": [0.05, 0.2]}))
    labels.apply_labels(rows, [], labels.load_config(custom))
    assert [r["label_fog_class"] for r in rows] == ["light", "dense", "dense", None]


def test_unknown_config_key_is_an_error(tmp_path):
    custom = tmp_path / "label_config.json"
    custom.write_text(json.dumps({"fog_treshold": [0.1, 0.2]}))  # typo
    with pytest.raises(ValueError):
        labels.load_config(custom)


def test_exclusions():
    rows = flat_rows([telemetry(1, T0), telemetry(2, T0 + 1000, state="SAFE"),
                      telemetry(3, T0 + 2000, radar_presence=True)])
    labels.apply_labels(rows, [], labels.load_config())
    assert [r["exclude_reason"] for r in rows] == ["", "state", "person_near"]


def test_a_new_labeler_adds_its_column():
    @labels.labeler("label_test")
    def test_label(rows, context):
        for row in rows:
            row["label_test"] = 1

    try:
        assert labels.label_columns()[-1] == "label_test"
        rows = flat_rows([telemetry(1, T0)])
        labels.apply_labels(rows, [], labels.load_config())
        assert rows[0]["label_test"] == 1
    finally:
        labels.LABELERS.pop()


# ------------------------------------------------------------------------------------------ tables

def test_segments_measure_each_fog_episode(tmp_path):
    lines = [line("event", event(1, T0, "FOG_DETECTED", {"fog_idx": 0.35, "source": "optical"}))]
    for s in range(1, 121):  # 2 minutes: 1 g/min, 0.01 Wh/min
        lines.append(line("telemetry", telemetry(s + 1, T0 + s * 1000, mass_g=s / 60, e_wh=s / 6000)))
    lines.append(line("event", event(200, T0 + 30_000, "ARC", {"peak_ua": 900, "hv_level": 0.8, "predicted": True})))
    lines.append(line("event", event(300, T0 + 120_000, "FOG_CLEARED", {"duration_s": 120})))
    run_dir = make_run(tmp_path, lines)
    dataset.build(run_dir)
    segment = read_csv(run_dir / "dataset" / "segments.csv")[0]
    assert segment["completed"] == "1"
    assert float(segment["duration_s"]) == 120.0
    assert abs(float(segment["mass_gain_g"]) - 1.9833) < 1e-3
    assert abs(float(segment["g_per_wh"]) - 100.0) < 0.5
    assert segment["arcs"] == "1"


def test_windows_are_labelled_from_their_trigger(tmp_path):
    def wave(seq, ts, trigger):
        return {"schema_version": ac.SCHEMA_VERSION, "node_id": "n01", "ts": ts, "seq": seq, "trigger": trigger,
                "fs_hz": 10000, "unit": "uA", "samples": [1.0, 2.0, 3.5]}

    run_dir = make_run(tmp_path, [
        line("wave", wave(1, T0, "ARC_PREDICTED")),
        line("wave", wave(2, T0 + 20_000, "ARC")),
        line("event", event(3, T0 + 20_026, "ARC", {"peak_ua": 900, "hv_level": 0.8, "predicted": False})),
    ])
    dataset.build(run_dir)
    windows = read_csv(run_dir / "dataset" / "windows.csv")
    assert [(w["label"], w["arc_followed"]) for w in windows] == [("pre_arc", "0"), ("arc", "1")]
    assert windows[0]["samples"] == "1 2 3.5"


def test_windows_followed_by_a_backoff_on_a_prediction_are_marked_intervened(tmp_path):
    def wave(seq, ts, trigger):
        return {"schema_version": ac.SCHEMA_VERSION, "node_id": "n01", "ts": ts, "seq": seq, "trigger": trigger,
                "fs_hz": 10000, "unit": "uA", "samples": [1.0, 2.0, 3.5]}

    run_dir = make_run(tmp_path, [
        line("wave", wave(1, T0, "MANUAL")),                       # a prediction 2 s later: intervened
        line("event", event(2, T0 + 2_000, "ARC_PREDICTED", {"risk": 0.8, "hv_level": 0.7})),
        line("wave", wave(3, T0 + 30_000, "MANUAL")),              # nothing after it: a clean "no arc"
        line("wave", wave(4, T0 + 60_000, "ARC_PREDICTED")),       # its own back-off: always intervened
    ])
    dataset.build(run_dir)
    windows = read_csv(run_dir / "dataset" / "windows.csv")
    assert [(w["trigger"], w["arc_followed"], w["intervened"]) for w in windows] == [
        ("MANUAL", "0", "1"), ("MANUAL", "0", "0"), ("ARC_PREDICTED", "0", "1")]


def test_full_simulated_run(tmp_path):
    sink = CollectSink()
    sim = Simulation(sink, nodes=3, seed=11, t0=T0 / 1000, wave=True, truth=True)
    sim.start()
    for _ in range(1800):
        sim.step()
    run_dir = make_run(tmp_path, [json.dumps({"topic": t, "retain": r, "payload": p}) for t, p, r in sink.messages])
    summary = dataset.build(run_dir)

    sent = sum(1 for t, _, _ in sink.messages if t.endswith("/telemetry"))
    assert summary["rows"]["telemetry"] == sent
    assert summary["input"]["invalid"] == summary["input"]["unreadable"] == 0
    tel = read_csv(run_dir / "dataset" / "telemetry.csv")
    assert list(tel[0].keys()) == ["run_id"] + dataset.telemetry_columns() + labels.label_columns()
    for node, s in summary["nodes"].items():
        detected = sum(1 for t, p, _ in sink.messages if t == f"aeroion/{node}/event" and p["type"] == "FOG_DETECTED")
        assert s["fog_episodes"] == detected
        assert s["lost_messages"] == 0 and s["restarts"] == 0
    assert summary["rows"]["segments"] >= 3
    assert json.loads((run_dir / "dataset" / "summary.json").read_text())["run_id"] == summary["run_id"]


# ------------------------------------------------------------------------------------------ recorder

def test_recorder_writes_contract_lines(tmp_path):
    raw = tmp_path / "raw.jsonl"
    rec = recorder.Recorder(raw)
    rec.handle("aeroion/n01/telemetry", json.dumps(telemetry(1, T0)).encode(), retain=False, rx_ms=T0 + 5)
    rec.handle("aeroion/n01/status", b"not json", retain=True, rx_ms=T0 + 6)
    rec.close()
    first, second = [json.loads(x) for x in raw.read_text().splitlines()]
    assert first["rx_ts"] == T0 + 5 and first["payload"]["seq"] == 1
    assert second["retain"] is True and second["payload"] == "not json"
    assert rec.counts == {"telemetry": 1, "status": 1}


# ------------------------------------------------------------------------------------------ command line

def run_cli(*args, cwd):
    env = dict(os.environ, PYTHONPATH=str(REPO))  # cwd is a temp folder, so say where `station` is
    return subprocess.run([sys.executable, "-m", "station.datalog", *args], cwd=cwd, capture_output=True,
                          text=True, env=env)


def test_cli_import_build_list(tmp_path):
    sim_file = tmp_path / "sim.jsonl"
    subprocess.run([sys.executable, str(REPO / "sim" / "fake_node.py"), "--out", str(sim_file), "--no-wait",
                    "--duration", "300", "--seed", "3"], check=True, capture_output=True)
    data = tmp_path / "runs"
    imported = run_cli("--data", str(data), "import", str(sim_file), "--tag", "sim", "--set", "seed=3", cwd=tmp_path)
    assert imported.returncode == 0, imported.stderr
    built = run_cli("--data", str(data), "build", "latest", cwd=tmp_path)
    assert built.returncode == 0, built.stdout + built.stderr
    assert "0 invalid" in built.stdout
    listed = run_cli("--data", str(data), "list", cwd=tmp_path)
    assert "built" in listed.stdout and '"seed":3' in listed.stdout
    missing = run_cli("--data", str(data), "build", "no-such-run", cwd=tmp_path)
    assert missing.returncode == 2


def test_cli_record_without_broker_leaves_nothing(tmp_path):
    with socket.socket() as s:  # a port where nothing listens
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    data = tmp_path / "runs"
    result = run_cli("--data", str(data), "record", "--port", str(port), "--duration", "1", cwd=tmp_path)
    assert result.returncode == 2
    assert "Cannot reach the MQTT broker" in result.stderr
    assert runs.list_runs(data) == []