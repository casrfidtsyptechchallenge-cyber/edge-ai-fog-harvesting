"""Tests for ml/arc_novelty.py (M5a novelty guard): it calls about the chosen share of normal windows novel,
catches the gross faults it is meant for, and the firmware's header is the one of the model in the firmware.
Skipped without numpy (requirements-ml.txt)."""

import re
from pathlib import Path

import pytest

np = pytest.importorskip("numpy")

from ml import arc_novelty as nv  # noqa: E402
from ml import arc_physics as phys  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
MODEL_H = REPO / "firmware" / "node" / "lib" / "edge_ai" / "arc_model_data.h"
NOVELTY_H = REPO / "firmware" / "node" / "lib" / "edge_ai" / "arc_novelty_data.h"


def deployed():
    name = re.search(r'#define ARC_MODEL_NAME "([^"]+)"', MODEL_H.read_text()).group(1)
    return name, REPO / "ml" / "models" / f"{name}.npz"


@pytest.fixture(scope="module")
def guard():
    _, path = deployed()
    q, rule, info, g, _ = nv.for_saved_model(path, log=lambda *a: None)
    return q, g


def test_the_header_belongs_to_the_model_in_the_firmware(guard):
    _, g = guard
    name, _ = deployed()
    text = NOVELTY_H.read_text()
    assert f'#define ARC_NOVELTY_MODEL "{name}"' in text
    thr = float(re.search(r"#define ARC_NOVELTY_THRESHOLD ([0-9.e+-]+)f", text).group(1))
    assert thr == pytest.approx(g.threshold, rel=1e-6)
    assert not re.search(r"(?<![0-9.e])\d+f", text.replace("ARC_", "")), "a float literal without a point"


def test_about_one_percent_of_normal_windows_are_called_novel(guard):
    q, g = guard
    X, _, _ = phys.make_dataset(12, 200, seed=8800)  # runs neither trained nor calibrated on
    share = g.novel(nv.features(q, X)).mean()
    assert share < 0.03


def test_a_disconnected_sensor_and_a_clipped_input_are_caught(guard):
    q, g = guard
    Xd, _ = nv.shifted_dataset(2, 100, 8801, {"disconnected": True})
    assert (g.why(nv.features(q, Xd)) == 2).all()
    Xc, _ = nv.shifted_dataset(4, 100, 8802, {"clip_ua": 60.0})
    clipped = (Xc == 60.0).sum(axis=1) >= nv.SATURATION_TIES
    assert clipped.any()
    assert (g.why(nv.features(q, Xc))[clipped] == 1).all()


def test_the_guard_survives_a_save_and_a_load(guard, tmp_path):
    q, g = guard
    path = tmp_path / "g.novelty.json"
    nv.save(g, path)
    g2, _ = nv.load(path)
    X, _, _ = phys.make_dataset(2, 50, seed=8803)
    f = nv.features(q, X)
    assert np.allclose(g.distance2(f), g2.distance2(f)) and (g.why(f) == g2.why(f)).all()


def test_the_shrunk_covariance_matches_the_reference_formula():
    sk = pytest.importorskip("sklearn.covariance")
    rng = np.random.default_rng(5)
    x = rng.normal(size=(400, 12)) @ rng.normal(size=(12, 12))
    x = x - x.mean(axis=0)
    assert np.allclose(nv.ledoit_wolf(x), sk.LedoitWolf(assume_centered=True).fit(x).covariance_, rtol=1e-8, atol=1e-10)
