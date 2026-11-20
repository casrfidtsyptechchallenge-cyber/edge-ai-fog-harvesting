"""Stand-in runs to try the training pipeline (M11) before the chamber exists. NOT measurements.

    python -m ml demo-runs                 4 runs of 30 min into data/demo-runs
    python -m ml pipeline --data data/demo-runs

Each run is what python -m station.datalog record would have kept from one node: the node simulator's
telemetry and events (sim/fake_node.py), plus a current window every 6 s drawn from the arc physics
(ml/arc_physics.py, as if the MCX sent `waves 6`), followed by an ARC event when the physics says an arc came
within 5 s. Half the runs hold the HV at 0.55 and half at 0.70 (station SET_HINTs), recorded with the setting
controller=fixed-0.55 or fixed-0.70, so the pipeline's controller table has two groups to compare.
They go to their own folder: never mix them with chamber runs.
"""

import json
import sys
import uuid
from pathlib import Path

import numpy as np

from . import arc_physics as phys

REPO = Path(__file__).resolve().parents[1]
DEMO = REPO / "data" / "demo-runs"
T0 = 1791747040000  # ms
WINDOW_EVERY_S = 6  # more than the 5 s horizon: each window's label is its own arc


def _simulator():
    for sub in ("contract", "sim"):
        path = str(REPO / sub)
        if path not in sys.path:
            sys.path.insert(0, path)
    import fake_node

    return fake_node


def make_run(data_dir, index, minutes=30, seed=1, level=None):
    """One run folder (run.json + raw.jsonl). Returns its path."""
    fake_node = _simulator()  # also makes `import aeroion_contract` work
    import aeroion_contract as ac
    from station.datalog import runs

    level = level if level is not None else (0.55 if index % 2 == 0 else 0.70)
    setting = f"fixed-{level:.2f}"
    sink = fake_node.CollectSink()
    t0 = T0 / 1000 + index * 86400
    sim = fake_node.Simulation(sink, nodes=1, seed=seed * 100 + index, t0=t0)
    sim.start()
    for s in range(minutes * 60):
        if s % 300 == 0:  # the station holds the level (SET_HINT, valid 10 min)
            sim.send_command("n01", {"schema_version": ac.SCHEMA_VERSION, "cmd_id": f"demo-{uuid.uuid4().hex[:8]}",
                                     "ts": int(sim.t * 1000), "ttl_s": 600, "type": "SET_HINT",
                                     "params": {"hv_level": level}})
        sim.step()
    sim.stop()
    messages = [(topic, payload, retain) for topic, payload, retain in sink.messages]

    # The current windows and their arcs, from the arc physics of one electrode.
    rng = np.random.default_rng(seed * 1000 + index)
    p = phys.draw_params(rng)
    t_ms = int(t0 * 1000) + 10000
    end_ms = int(t0 * 1000) + minutes * 60000 - 6000
    while t_ms < end_ms:
        v, fog = phys.draw_condition(p, rng)
        x = phys.window(p, v, fog, rng)
        messages.append((ac.node_topic("n01", "wave"), {
            "schema_version": ac.SCHEMA_VERSION, "node_id": "n01", "ts": t_ms, "seq": 0, "trigger": "MANUAL",
            "fs_hz": phys.FS_HZ, "unit": "uA", "samples": [round(float(s), 1) for s in x]}, False))
        if rng.random() < phys.arc_probability(p, v, fog):
            messages.append((ac.node_topic("n01", "event"), {
                "schema_version": ac.SCHEMA_VERSION, "node_id": "n01", "ts": t_ms + int(rng.uniform(500, 4500)),
                "seq": 0, "type": "ARC",
                "data": {"peak_ua": round(float(phys.baseline_ua(p, v, fog)) * 20, 1),
                         "hv_level": round(float(min(v, 1.0)), 3), "predicted": False}}, False))
        t_ms += WINDOW_EVERY_S * 1000

    # One counter for the node, in time order, as the ESP32 numbers them.
    numbered = [m for m in messages if "seq" in m[1]]
    numbered.sort(key=lambda m: m[1]["ts"])
    for i, (_, payload, _) in enumerate(numbered, 1):
        payload["seq"] = i
    lines = [json.dumps({"topic": topic, "retain": retain, "payload": payload}, separators=(",", ":"))
             for topic, payload, retain in sorted(messages, key=lambda m: m[1].get("ts", 0))]
    run_dir, _ = runs.new_run(data_dir, tag=f"demo{index}", settings={"controller": setting, "source": "demo"},
                              notes="stand-in run: simulator + arc physics, not a measurement", source="import")
    (run_dir / runs.RAW_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return run_dir


def make_runs(data_dir=DEMO, count=4, minutes=30, seed=1, log=print):
    data_dir = Path(data_dir)
    data_dir.mkdir(parents=True, exist_ok=True)
    made = []
    for i in range(count):
        made.append(make_run(data_dir, i, minutes, seed))
        log(f"  {made[-1].name}")
    return made