"""Tests for the training pipeline (ml/pipeline.py, M11) and its stand-in runs (ml/demo_runs.py): from run
folders to a retrained arc predictor, the firmware headers written only when they should be, the node's models
checked, the controller table, the report. Skipped without numpy (requirements-ml.txt)."""

import argparse
import json
import re

import pytest

np = pytest.importorskip("numpy")

from ml import arc_cnn as cnn  # noqa: E402
from ml import arc_physics as phys  # noqa: E402
from ml import demo_runs, pipeline  # noqa: E402


def parse_c_array(text, name):
    body = re.search(name + r"\[[^\]]*\](?:\[[^\]]*\])? = \{(.*?)\};", text, re.S).group(1)
    return [float(v.rstrip("f")) for v in re.findall(r"-?[0-9.]+(?:e-?[0-9]+)?f?", body)]


@pytest.fixture(scope="module")
def runs_dir(tmp_path_factory):
    data = tmp_path_factory.mktemp("runs")
    demo_runs.make_runs(data, count=3, minutes=12, seed=5, log=lambda *_: None)
    return data


def settings(tmp_path, data, **overrides):
    args = argparse.Namespace(data=str(data), runs=[], synthetic_runs=4, epochs=2, seed=1, threshold=0.30,
                              min_test=50, compare_by="controller", force=False, dry_run=False, name=None,
                              out=str(tmp_path / "arc_model_data.h"), test_out=str(tmp_path / "arc_model_test.h"),
                              reports=str(tmp_path / "reports"), models=str(tmp_path / "models"))
    for key, value in overrides.items():
        setattr(args, key, value)
    return args


def quiet(*_):
    pass


def test_the_stand_in_runs_are_runs_like_recorded_ones(runs_dir):
    dirs = pipeline.run_dirs(runs_dir)
    assert len(dirs) == 3
    pipeline.ensure_datasets(dirs, quiet)
    for d in dirs:
        summary = json.loads((d / "dataset" / "summary.json").read_text(encoding="utf-8"))
        assert summary["input"]["invalid"] == 0 and summary["input"]["unreadable"] == 0  # every message valid
        assert summary["nodes"]["n01"]["lost_messages"] == 0  # one counter, in time order
        windows = pipeline.read_table(d, "windows.csv")
        assert len(windows) > 100 and all(w["trigger"] == "MANUAL" for w in windows)
        assert 0.05 < np.mean([int(w["arc_followed"]) for w in windows]) < 0.6
    assert {pipeline.settings_of(d)["controller"] for d in dirs} == {"fixed-0.55", "fixed-0.70"}
    w, skipped = pipeline.load_windows(dirs)
    train, test = pipeline.split(w)
    assert w["X"].shape[1] == phys.WINDOW and not skipped
    assert set(w["run"][test]) == {2} and set(w["run"][train]) == {0, 1}  # tested on the newest run only


def test_windows_with_a_backoff_after_them_are_not_training_labels(tmp_path):
    """Intervention bias: after a back-off on a prediction, "no arc followed" proves nothing."""
    d = tmp_path / "run"
    (d / "dataset").mkdir(parents=True)
    samples = " ".join(["10"] * phys.WINDOW)
    rows = [("MANUAL", 0, 0), ("MANUAL", 0, 1), ("ARC_PREDICTED", 0, 1), ("MANUAL", 1, 1), ("ARC_PREDICTED", 0, "")]
    with open(d / "dataset" / "windows.csv", "w", encoding="utf-8") as f:
        f.write("run_id,node_id,ts,seq,trigger,label,arc_followed,intervened,fs_hz,n_samples,samples\n")
        for i, (trig, followed, intervened) in enumerate(rows):
            f.write(f"r,n01,{1000 * i},{i},{trig},x,{followed},{intervened},10000,{phys.WINDOW},{samples}\n")
    w, skipped = pipeline.load_windows([d])
    assert list(w["y"]) == [0]  # only the clean "no arc"
    assert skipped["a back-off followed (outcome unknown)"] == 4  # an old dataset's ARC_PREDICTED row too


def test_the_pipeline_retrains_and_writes_the_firmware_headers(runs_dir, tmp_path):
    args = settings(tmp_path, runs_dir, name="arc-test-1")
    report, code = pipeline.run(args, quiet)
    assert code == 0
    text = report.read_text(encoding="utf-8")
    for section in ("## Arc predictor (M5a)", "## Virtual fog sensor (M5b)", "## HV controller (M6)",
                    "stand-in runs"):
        assert section in text
    assert "no model to compare with" in text and "headers were written" in text
    header = (tmp_path / "arc_model_data.h").read_text(encoding="utf-8")
    assert '#define ARC_MODEL_NAME "arc-test-1"' in header and "python -m ml pipeline" in header
    q, rule, info = cnn.load_model(tmp_path / "models" / "arc-test-1.npz")
    assert info["exported"] and info["tests"][0]["auc"] > 0.8  # learned something, even in 2 epochs
    assert parse_c_array(header, "kArcW2") == q.w2.ravel().tolist()
    golden = (tmp_path / "arc_model_test.h").read_text(encoding="utf-8")  # bit-exact with the saved model
    windows = np.array(parse_c_array(golden, "kArcTestUa"), dtype=np.float32).reshape(-1, phys.WINDOW)
    z, _, _ = cnn.preprocess(windows)
    acc, _ = q.accumulate(cnn.quantize_input(z))
    assert acc.tolist() == parse_c_array(golden, "kArcTestSum")
    risk = q.predict(z)
    assert len(windows) == 4 and (np.diff(risk) >= 0).all()  # from the safest to the riskiest


def test_the_firmware_model_stays_unless_the_new_one_is_better_or_forced(runs_dir, tmp_path):
    pipeline.run(settings(tmp_path, runs_dir, name="arc-test-a"), quiet)
    header = tmp_path / "arc_model_data.h"
    first = header.read_text(encoding="utf-8")
    report, _ = pipeline.run(settings(tmp_path, runs_dir, name="arc-test-b", dry_run=True), quiet)
    text = report.read_text(encoding="utf-8")
    assert "firmware's CNN (arc-test-a)" in text  # compared with the model in the firmware
    assert "(--dry-run)" in text and header.read_text(encoding="utf-8") == first
    report, _ = pipeline.run(settings(tmp_path, runs_dir, name="arc-test-c", min_test=100000), quiet)
    assert "too few test windows" in report.read_text(encoding="utf-8")
    assert header.read_text(encoding="utf-8") == first
    pipeline.run(settings(tmp_path, runs_dir, name="arc-test-d", min_test=100000, force=True), quiet)
    assert '"arc-test-d"' in header.read_text(encoding="utf-8")
    worse = settings(tmp_path, runs_dir, name="arc-test-e", epochs=1, synthetic_runs=0, seed=9)
    report, _ = pipeline.run(worse, quiet)  # 1 epoch, no synthetic data: does it beat the firmware's model?
    _, _, info = cnn.load_model(tmp_path / "models" / "arc-test-e.npz")
    new, old = info["tests"][0]["log_loss"], info["tests"][2]["log_loss"]
    assert info["exported"] == (new < old)
    assert ('"arc-test-e"' in header.read_text(encoding="utf-8")) == (new < old)


def test_without_windows_nothing_is_trained_and_the_rest_is_reported(tmp_path):
    from fake_node import CollectSink, Simulation
    from station.datalog import runs

    sink = CollectSink()
    sim = Simulation(sink, nodes=1, seed=3, t0=1791747040.0)
    sim.start()
    for _ in range(600):
        sim.step()
    run_dir, _ = runs.new_run(tmp_path / "data", tag="nowave", settings={"controller": "sim"}, source="import")
    lines = [json.dumps({"topic": t, "retain": r, "payload": p}) for t, p, r in sink.messages]
    (run_dir / runs.RAW_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")
    report, code = pipeline.run(settings(tmp_path, tmp_path / "data"), quiet)
    text = report.read_text(encoding="utf-8")
    assert code == 0 and "No recorded windows" in text and "| sim | 1 |" in text
    assert not (tmp_path / "arc_model_data.h").exists()


def test_the_controller_table_and_the_node_models_check(runs_dir, tmp_path):
    dirs = pipeline.run_dirs(runs_dir)
    table = "\n".join(pipeline.controller_step(dirs, "controller"))
    assert "| fixed-0.55 | 2 |" in table and "| fixed-0.70 | 1 |" in table
    checks = "\n".join(pipeline.node_models_step(dirs))
    rows = [line for line in checks.splitlines() if line.startswith("| 20")]
    assert len(rows) == 3
    error = float(rows[0].split("|")[5])  # the simulator's virtual fog is close to its optical fog
    assert 0 <= error < 0.1


def test_models_lists_the_versions_marks_the_firmware_one_and_puts_one_back(runs_dir, tmp_path):
    pipeline.run(settings(tmp_path, runs_dir, name="arc-test-x"), quiet)
    out = []
    assert pipeline.list_models(tmp_path / "models", tmp_path / "arc_model_data.h", out.append) == 0
    assert any("arc-test-x" in line and "in the firmware" in line for line in out)
    assert pipeline.list_models(tmp_path / "empty", tmp_path / "arc_model_data.h", out.append) == 2
    pipeline.run(settings(tmp_path, runs_dir, name="arc-test-y", force=True), quiet)
    header = tmp_path / "arc_model_data.h"
    assert '"arc-test-y"' in header.read_text(encoding="utf-8")
    assert pipeline.deploy("arc-test-x", tmp_path / "models", header, tmp_path / "arc_model_test.h", log=quiet) == 0
    assert '"arc-test-x"' in header.read_text(encoding="utf-8")  # back to the earlier version
    q, _, _ = cnn.load_model(tmp_path / "models" / "arc-test-x.npz")
    assert parse_c_array(header.read_text(encoding="utf-8"), "kArcW2") == q.w2.ravel().tolist()
    assert pipeline.deploy("nope", tmp_path / "models", header, tmp_path / "arc_model_test.h", log=quiet) == 2