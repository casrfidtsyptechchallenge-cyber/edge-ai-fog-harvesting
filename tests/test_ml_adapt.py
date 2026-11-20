"""Tests for ml/arc_adapt.py (experiment, not in the firmware): the auto-labels keep only unbiased windows, the
fit stays near the factory weights with little data, the gates refuse what is not clearly better, FedAvg weighs
by data. Skipped without numpy."""
import pytest

np = pytest.importorskip("numpy")

from ml import arc_adapt as ad  # noqa: E402


def log_of(t, probe, arcs, actions, end):
    rng = np.random.default_rng(0)
    return {"t": np.asarray(t, float), "features": rng.integers(0, 60, (len(t), 32)).astype(float),
            "probe": np.asarray(probe, bool), "arcs": arcs, "actions": actions, "end": end}


def test_only_probe_windows_are_labels_and_back_offs_censor():
    # windows at 0..9 s; a probe from 5 s; an arc at 8.5 s; a back-off at 2.5 s
    lg = log_of(range(10), [False] * 5 + [True] * 5, arcs=[8.5], actions=[2.5, 8.5], end=20.0)
    F, y, t, pr = ad.auto_label(lg)
    assert list(t) == [5, 6, 7, 8, 9] and list(y) == [1, 1, 1, 1, 0] and pr.all()
    # without the unbiased rule, censoring alone keeps the "safe" windows outside probes too
    _, y2, t2, _ = ad.auto_label(lg, censor=True, unbiased=False)
    assert list(t2) == [3, 4, 5, 6, 7, 8, 9]   # 0, 1, 2: the back-off at 2.5 s came within 5 s: censored


def test_with_little_data_the_fit_stays_near_the_factory():
    rng = np.random.default_rng(1)
    start = ad.LastLayer(rng.normal(0, 0.05, 32), -3.0)
    F = rng.integers(0, 60, (30, 32)).astype(float)
    y = np.zeros(30)
    scale = F.std(axis=0) + 1.0
    new = ad.fit(F, y, start, scale)
    assert np.isfinite(new.theta()).all()
    assert abs(new.b - start.b) < 2.0          # all-negative data cannot run the bias off


def test_the_gate_refuses_a_change_within_noise():
    rng = np.random.default_rng(2)
    F = rng.integers(0, 60, (200, 32)).astype(float)
    y = (rng.random(200) < 0.1).astype(float)
    a = ad.LastLayer(np.zeros(32), -2.2)
    b = ad.LastLayer(np.zeros(32), -2.19)      # the same model, give or take
    assert not ad.gate(a, b, F, y)
    worse = ad.LastLayer(np.zeros(32), 1.0)    # warns all the time
    assert ad.gate(worse, a, F, y) and not ad.gate(a, worse, F, y)


def test_the_trust_region_is_asymmetric_the_safe_way():
    F = np.ones((10, 32))
    old = ad.LastLayer(np.zeros(32), 0.0)
    up = ad.shrink_to_trust(old, ad.LastLayer(np.zeros(32), 5.0), F)
    down = ad.shrink_to_trust(old, ad.LastLayer(np.zeros(32), -5.0), F)
    assert up.b == pytest.approx(ad.MAX_UP) and down.b == pytest.approx(-ad.MAX_DOWN)


def test_fedavg_weighs_by_data():
    a, b = ad.LastLayer(np.zeros(32), 0.0), ad.LastLayer(np.ones(32), 4.0)
    m = ad.fedavg([a, b], [300, 100])
    assert m.b == pytest.approx(1.0) and np.allclose(m.w, 0.25)
