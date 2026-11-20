"""Tests for ml/ (M5a, laptop side): the synthetic windows, the CNN and its 8 bits, the rule, the export, and
that the firmware's generated headers match the saved model. Skipped without numpy (requirements-ml.txt)."""

import re
from pathlib import Path

import pytest

np = pytest.importorskip("numpy")

from ml import arc_cnn as cnn  # noqa: E402
from ml import arc_eval as ev  # noqa: E402
from ml import arc_physics as phys  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
MODEL_H = REPO / "firmware" / "node" / "lib" / "edge_ai" / "arc_model_data.h"
TEST_H = REPO / "firmware" / "node" / "test" / "test_edge_ai" / "arc_model_test.h"


def test_the_synthetic_windows_carry_the_physics():
    rng = np.random.default_rng(0)
    p = phys.draw_params(rng)
    fog = 0.6
    far = phys.sparkover(p, fog) - 0.3
    near = phys.sparkover(p, fog) - 0.01
    assert phys.arc_probability(p, far, fog) < 0.01 < 0.5 < phys.arc_probability(p, near, fog)
    w_far = phys.window(p, far, fog, rng)
    assert w_far.shape == (phys.WINDOW,) and (w_far >= 0).all()
    assert abs(np.median(w_far) - phys.baseline_ua(p, far, fog)) < 0.1 * phys.baseline_ua(p, far, fog)
    near_windows = np.array([phys.window(p, near - 0.02, fog, rng) for _ in range(40)])
    far_windows = np.array([phys.window(p, far, fog, rng) for _ in range(40)])
    z_far, s_far, _ = cnn.preprocess(far_windows)
    z_near, s_near, _ = cnn.preprocess(near_windows)
    assert cnn.pulse_rate(z_near, s_near).mean() > 5 * cnn.pulse_rate(z_far, s_far).mean() + 1
    X, y, meta = phys.make_dataset(3, 50, seed=1)
    assert X.shape == (150, phys.WINDOW) and set(np.unique(y)) <= {0, 1}
    assert (meta[:, 3] >= 0).all() and (meta[:, 3] <= 1).all()


def test_preprocessing_and_pulse_counting():
    x = np.full((1, 256), 50.0, dtype=np.float32)
    x[0, ::2] += 0.5                       # noise: alternating ±0.25 around 50.25
    x[0, 100:103] += 20.0                  # one pulse of 3 samples
    x[0, 200] += 30.0                      # one lone spike: interference, not a pulse
    z, sigma, median = cnn.preprocess(x)
    assert median[0] == pytest.approx(50.25, abs=0.3)
    assert z.max() <= 1.0 and z.min() >= -1.0
    assert cnn.pulse_rate(z, sigma)[0] == pytest.approx(1 * phys.FS_HZ / 256)
    q = cnn.quantize_input(np.array([[0.5 / 127, -0.5 / 127, 1.0, -1.0]]))
    assert q.tolist() == [[1, -1, 127, -127]]  # half away from zero, as the C++


def test_the_integer_requantisation():
    for real in (0.0007, 0.013, 0.42):
        m, shift = cnn._multiplier(real)
        assert 2 ** 30 <= m < 2 ** 31
        acc = np.array([-100_000, -1, 0, 1, 12_345, 600_000], dtype=np.int64)
        expected = np.clip(np.floor(acc * real + 0.5), -128, 127)
        assert np.abs(cnn._requant(acc, m, shift) - expected).max() <= 1


def test_training_learns_and_8_bits_lose_nothing():
    X, y, _ = phys.make_dataset(12, 120, seed=5)
    Xv, yv, _ = phys.make_dataset(4, 120, seed=6)
    z, sig, med = cnn.preprocess(X)
    zv, sv, mv = cnn.preprocess(Xv)
    net = cnn.ArcNet(seed=0)
    before = cnn.log_loss(net.predict(zv), yv)
    after = cnn.train(net, z, y, zv, yv, epochs=4, log=lambda s: None)
    assert after < before
    q = cnn.QuantArcNet(net, z)
    assert ev.auc(q.predict(zv), yv) > 0.8
    assert np.abs(q.predict(zv) - net.predict(zv)).max() < 0.1
    rule = cnn.fit_rule(z, sig, med, y)
    assert ev.auc(cnn.rule_predict(zv, sv, mv, rule), yv) > 0.7


def test_auc_by_ranks():
    assert ev.auc([0.1, 0.2, 0.8, 0.9], [0, 0, 1, 1]) == 1.0
    assert ev.auc([0.9, 0.8, 0.2, 0.1], [0, 0, 1, 1]) == 0.0
    assert ev.auc([0.5, 0.5, 0.5, 0.5], [0, 1, 0, 1]) == 0.5


def test_a_predictor_prevents_arcs_in_the_closed_loop():
    none = ev.closed_loop(None, 1.0, runs=2, minutes=6, seed=3)
    always = ev.closed_loop(lambda w: 1.0, 0.5, runs=2, minutes=6, seed=3)  # warns all the time
    assert none["arcs_per_h"] > 0 and none["warnings_per_h"] == 0
    assert always["arcs_per_h"] < none["arcs_per_h"] and always["warnings_per_h"] > 1000


def parse_c_array(text, name):
    m = re.search(rf"{name}\[[^\]]*\](?:\[[^\]]*\])? = \{{(.*?)\}};", text, re.S)
    assert m, name
    return [float(v.rstrip("f")) for v in re.findall(r"-?[\d.]+(?:e-?\d+)?f?", m.group(1))]


@pytest.mark.skipif(not MODEL_H.exists() or not TEST_H.exists(), reason="python -m ml arc-train not run yet")
def test_the_firmware_headers_match_the_saved_model():
    text, test = MODEL_H.read_text(encoding="utf-8"), TEST_H.read_text(encoding="utf-8")
    name = re.search(r'#define ARC_MODEL_NAME "([^"]+)"', text).group(1)
    path = REPO / "ml" / "models" / f"{name}.npz"
    if not path.exists():
        pytest.skip(f"ml/models/{name}.npz is not here (trained elsewhere)")
    q, rule, info = cnn.load_model(path)
    assert parse_c_array(text, "kArcW2") == q.w2.ravel().tolist()
    windows = np.array(parse_c_array(test, "kArcTestUa"), dtype=np.float32).reshape(-1, phys.WINDOW)
    z, sigma, median = cnn.preprocess(windows)
    q0 = cnn.quantize_input(z)
    assert q0.ravel().tolist() == parse_c_array(test, "kArcTestInput")
    acc, feat = q.accumulate(q0)
    assert acc.tolist() == parse_c_array(test, "kArcTestSum")
    assert feat.ravel().tolist() == parse_c_array(test, "kArcTestFeatures")