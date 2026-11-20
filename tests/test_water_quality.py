"""Tests for station/water/quality.py (M9, F7): the quality gate that lets the valve open."""

from station.water.hardware import ph_from_volts, tds_from_volts
from station.water.quality import Limits, QualityGate, Reading


def feed(gate, values, start_ms=0, step_ms=1000):
    """values: [(tds, ph), ...] one per second. Returns the verdicts."""
    out = []
    for i, (tds, ph) in enumerate(values):
        t = start_ms + i * step_ms
        gate.add(Reading(t, tds, ph, 15.0))
        out.append(gate.evaluate(t))
    return out


def test_good_water_is_ok_only_after_it_settles():
    gate = QualityGate()
    verdicts = feed(gate, [(90, 7.0)] * 5)
    assert [v.verdict for v in verdicts] == ["unknown", "unknown", "unknown", "ok", "ok"]
    assert verdicts[1].reason == "readings good, checking (1 of 3 s)"
    assert (verdicts[-1].tds_ppm, verdicts[-1].ph, verdicts[-1].temp_c, verdicts[-1].reason) == (90, 7.0, 15.0, "")


def test_bad_water_closes_within_two_readings():
    gate = QualityGate()
    feed(gate, [(90, 7.0)] * 4)
    verdicts = feed(gate, [(940, 7.0), (950, 7.0)], start_ms=4000)
    assert [v.verdict for v in verdicts] == ["ok", "bad"]
    assert verdicts[-1].reason == "TDS 940 ppm, above 500"


def test_one_wrong_reading_changes_nothing():
    gate = QualityGate()
    feed(gate, [(90, 7.0)] * 4)
    verdicts = feed(gate, [(3000, 2.0), (91, 7.0), (92, 7.1)], start_ms=4000)
    assert [v.verdict for v in verdicts] == ["ok", "ok", "ok"]


def test_ph_limits_both_ways_and_the_way_back():
    gate = QualityGate()
    feed(gate, [(90, 7.0)] * 4)
    low = feed(gate, [(90, 5.6)] * 2, start_ms=4000)[-1]
    assert (low.verdict, low.reason) == ("bad", "pH 5.6, below 6.5")
    high = feed(gate, [(90, 9.1)] * 2, start_ms=6000)[-1]
    assert (high.verdict, high.reason) == ("bad", "pH 9.1, above 8.5")
    back = feed(gate, [(90, 7.0)] * 6, start_ms=8000)
    assert [v.verdict for v in back] == ["bad", "bad", "bad", "bad", "ok", "ok"]  # the median, then 3 s good
    assert back[2].reason.startswith("readings good, checking")


def test_silent_or_missing_probes_mean_unknown():
    gate = QualityGate()
    feed(gate, [(90, 7.0)] * 4)
    assert gate.evaluate(3000 + 5000).verdict == "ok"
    v = gate.evaluate(3000 + 7001)  # no reading for 5 s and more
    assert (v.verdict, v.reason) == ("unknown", "no reading from the probes")
    v = feed(gate, [(None, 7.0)] * 3, start_ms=20_000)[-1]
    assert (v.verdict, v.reason) == ("unknown", "no TDS reading")
    v = feed(gate, [(90, float("nan"))] * 3, start_ms=30_000)[-1]
    assert (v.verdict, v.reason) == ("unknown", "no pH reading")
    v = feed(gate, [(90, 15.2)] * 3, start_ms=40_000)[-1]  # outside 0-14: a broken probe, not a pH
    assert v.verdict == "unknown" and v.ph is None


def test_limits_can_be_set():
    gate = QualityGate(Limits(tds_max_ppm=300, settle_s=0))
    assert feed(gate, [(350, 7.0)] * 2)[-1].reason == "TDS 350 ppm, above 300"
    assert feed(gate, [(250, 7.0)] * 2, start_ms=2000)[-1].verdict == "ok"


def test_probe_formulas():
    assert round(tds_from_volts(1.0), 1) == 367.5                      # DFRobot's curve at 25 °C
    assert tds_from_volts(1.0, temp_c=15.0) > tds_from_volts(1.0)      # cold water conducts less: corrected up
    assert tds_from_volts(0.0) == 0.0
    assert ph_from_volts(1.500, 1.500, 2.032) == 7.0
    assert round(ph_from_volts(2.032, 1.500, 2.032), 6) == 4.0
    assert round(ph_from_volts(1.234, 1.500, 2.032), 2) == 8.5          # the line goes on above 7