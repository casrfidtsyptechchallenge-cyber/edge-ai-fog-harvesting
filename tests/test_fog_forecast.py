"""Tests for ml/fog_forecast.py: reading IEM METAR files, the dew point, no look-ahead in the features, the split
by year, and that the model finds a fog signal planted in synthetic reports. These test the MACHINERY: the
results that count come from real METAR files (python -m ml fog-forecast)."""
import math
from datetime import datetime, timedelta

import pytest

np = pytest.importorskip("numpy")

from ml import fog_forecast as ff  # noqa: E402


def write_iem(path, years=3, seed=0, missing_every=97):
    """Hourly reports with a planted mechanism: fog forms on calm nights when the dew point depression has been
    closing; otherwise clear. Written exactly as the IEM 'onlycomma' format."""
    rng = np.random.default_rng(seed)
    t0 = datetime(2021, 1, 1)
    fog_left, evening = 0, 4.0
    with open(path, "w", encoding="utf-8") as f:
        f.write("station,valid,tmpc,dwpc,relh,sknt,drct,vsby\n")
        for i in range(years * 365 * 24):
            t = t0 + timedelta(hours=i)
            if t.hour == 18:
                evening = rng.uniform(1.0, 7.0)          # each evening its own dew point depression
            hours_in = (t.hour - 18) % 24                # hours since 18:00
            night = t.hour >= 21 or t.hour <= 6
            wind = max(0.0, rng.normal(4, 3))
            spread = max(0.0, evening - 0.5 * min(hours_in, 12) + rng.normal(0, 0.3)) if hours_in < 14 else \
                evening + 2.0
            if fog_left == 0 and night and spread < 1.0 and wind < 3 and rng.random() < 0.5:
                fog_left = int(rng.integers(2, 6))
            vis = rng.uniform(0.1, 0.5) if fog_left else rng.uniform(3, 10)
            fog_left = max(0, fog_left - 1)
            tc = 12 + 6 * math.sin(2 * math.pi * (t.hour - 9) / 24) + rng.normal(0, 0.5)
            dc = tc - spread
            vals = [f"{tc:.1f}", f"{dc:.1f}", "", f"{wind:.0f}", "270", f"{vis:.2f}"]
            if i % missing_every == 0:
                vals = ["M"] * 6
            f.write(f"DTXX,{t:%Y-%m-%d %H:%M}," + ",".join(vals) + "\n")


def test_the_dew_point_matches_a_known_value():
    assert ff.dew_point(20.0, 50.0) == pytest.approx(9.3, abs=0.1)
    assert ff.dew_point(10.0, 100.0) == pytest.approx(10.0, abs=0.05)


def test_reading_skips_missing_values(tmp_path):
    p = tmp_path / "m.csv"
    write_iem(p, years=1)
    st = ff.read_iem([p])
    assert list(st) == ["DTXX"]
    rows = st["DTXX"]
    assert len(rows) == 365 * 24
    assert rows[0][1]["t_c"] is None and rows[1][1]["t_c"] is not None   # row 0 is all 'M'


def test_no_feature_looks_ahead(tmp_path):
    p = tmp_path / "m.csv"
    write_iem(p, years=1)
    g = ff.hourly(ff.read_iem([p])["DTXX"])
    X1, y1, *_ = ff.build(g, 3)
    g2 = {k: v.copy() for k, v in g.items()}
    cut = len(g2["t_c"]) // 2
    for k in ("t_c", "dew_c", "wind_kt"):            # change the second half: the first half's features stay
        g2[k][cut:] = g2[k][cut:] + 5.0
    X2, *_ = ff.build(g2, 3)
    n = (X1["t_c"] < 1e9).sum()
    first = slice(0, n // 3)
    for k in ff.BASE_FEATURES + ff.WIND_FEATURES:
        assert np.allclose(X1[k][first], X2[k][first], equal_nan=True), k


def test_the_target_is_onset_only_and_the_test_year_is_unseen(tmp_path):
    p = tmp_path / "m.csv"
    write_iem(p, years=3)
    report = ff.run([p], log=lambda *a: None)
    for h in ("1h", "3h"):
        r = report["horizons"][h]
        assert r["test_year"] == 2023 and r["train_rows"] > r["test_rows"] > 0
        assert r["fog_onsets_in_test"] > 10
        node = r["node model (T, RH, time)"]
        assert node["auc"] > r["climatology (hour x month)"]["auc"]   # the planted signal is found
        assert node["auc"] > 0.8
