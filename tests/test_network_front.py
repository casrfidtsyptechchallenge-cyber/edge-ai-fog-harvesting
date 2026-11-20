"""Tests for station/network/front.py and tracker.py (M8): the fog front from the nodes' detection times."""

import math

import pytest

import aeroion_contract as ac
from station.network.front import angle_error, direction_of, fit_front, slowness
from station.network.tracker import FrontTracker, Prior, TrackerSettings

T0 = 1791747040000
TRIANGLE = {"n01": (0.0, 0.0), "n02": (15.0, 12.0), "n03": (30.0, 0.0), "n04": (45.0, 12.0)}


def arrivals(direction, speed, nodes=TRIANGLE, t0=100.0):
    wx, wy = slowness(direction, speed)
    return {n: t0 + wx * x + wy * y for n, (x, y) in nodes.items()}


def detections(times, names):
    return [(*TRIANGLE[n], times[n]) for n in names]


# ------------------------------------------------------------------ the fit


def test_directions_follow_the_contract():
    assert direction_of(*slowness(0, 1)) == pytest.approx(0)        # moving north
    assert direction_of(*slowness(90, 1)) == pytest.approx(90)      # moving east
    assert direction_of(*slowness(225, 2)) == pytest.approx(225)
    assert direction_of(*slowness(359.99, 1)) == 0.0                # never 360 (the schema excludes it)
    assert angle_error(350, 10) == 20 and angle_error(90, 270) == 180


def test_three_nodes_give_the_direction_and_speed():
    times = arrivals(72.5, 1.1)
    fit = fit_front(detections(times, ["n01", "n02", "n03"]))
    assert fit.basis == "fit" and fit.nodes == 3 and fit.quality is None  # 3 equations, 3 unknowns: no check
    assert fit.direction_deg == pytest.approx(72.5, abs=1e-6) and fit.speed_mps == pytest.approx(1.1)
    assert fit.arrival(*TRIANGLE["n04"]) == pytest.approx(times["n04"])


def test_four_nodes_also_say_how_well_a_straight_front_fits():
    times = arrivals(200, 0.8)
    assert fit_front(detections(times, TRIANGLE)).quality == pytest.approx(1.0)
    times["n04"] += 20  # one node much later than a straight front allows
    fit = fit_front(detections(times, TRIANGLE))
    assert fit.quality < 0.9


def test_two_nodes_take_the_rest_from_the_prior():
    times = arrivals(60, 1.0)
    assert fit_front(detections(times, ["n01", "n03"])) is None  # along the line or across it: unknown
    fit = fit_front(detections(times, ["n01", "n03"]), prior=slowness(60, 1.0))
    assert fit.basis == "line" and fit.direction_deg == pytest.approx(60) and fit.speed_mps == pytest.approx(1.0)
    wrong = fit_front(detections(times, ["n01", "n03"]), prior=slowness(0, 1.0))
    assert wrong.wx == pytest.approx(slowness(60, 1.0)[0])  # along the line (x), the data win over the prior


def test_nodes_on_one_line_count_as_two():
    line = {"a": (0, 0), "b": (10, 0.5), "c": (20, 0)}
    times = arrivals(30, 1.0, line)
    assert fit_front([(*line[n], times[n]) for n in line]) is None


def test_one_node_and_the_prior():
    assert fit_front([(0, 0, 50.0)]) is None
    fit = fit_front([(15.0, 12.0, 50.0)], prior=slowness(90, 2.0))
    assert fit.basis == "prior" and fit.arrival(15.0, 12.0) == pytest.approx(50.0)
    assert fit.arrival(30.0, 12.0) == pytest.approx(57.5)  # 15 m further east at 2 m/s


def test_fog_everywhere_at_once_has_no_direction():
    fit = fit_front([(0, 0, 10.0), (15, 12, 10.0), (30, 0, 10.2)])
    assert fit.direction_deg is None and fit.speed_mps is None
    assert fit.arrival(45, 12) == pytest.approx(10.0667, abs=1e-3)  # due now everywhere


# ------------------------------------------------------------------ the tracker


def tracker(states=None, prior=None, **settings):
    t = FrontTracker(TrackerSettings(**settings), prior)
    for node, (x, y) in TRIANGLE.items():
        t.set_position(node, x, y)
        t.set_synced(node, True)
        t.set_state(node, (states or {}).get(node, "ARMED"))
    return t


def outputs(out, kind):
    return [o for o in out if o[0] == kind]


def valid(message):
    assert ac.validate("front", message) == [], message
    return message


def test_the_first_detection_pre_arms_the_armed_nodes_by_distance():
    t = tracker(states={"n04": "LOCKED"})
    out = t.detected("n01", T0, T0 + 500)
    prearms = {node: eta for _, node, eta, src in outputs(out, "prearm")}
    assert prearms == {"n02": pytest.approx(math.hypot(15, 12) / 0.5 - 0.5, abs=0.1),
                       "n03": pytest.approx(30 / 0.5 - 0.5, abs=0.1)}  # n04 is LOCKED: nothing to pre-arm
    assert all(src == "n01" for *_, src in outputs(out, "prearm"))
    (_, msg), = outputs(out, "front")
    assert valid(msg)["active"] is True and msg["direction_deg"] is None and msg["eta_s"] == {}
    assert msg["nodes_detected"] == ["n01"] and t.front.not_armed == {"n04"}


def test_three_detections_measure_the_front_and_time_the_others():
    times = arrivals(80, 1.2, t0=0)
    t = tracker()
    base = T0 - int(min(times.values()) * 1000)
    order = sorted(["n01", "n02", "n03"], key=times.get)
    for node in order:
        out = t.detected(node, base + int(times[node] * 1000), base + int(times[node] * 1000))
    (_, msg), = outputs(out, "front")
    assert valid(msg)["direction_deg"] == pytest.approx(80, abs=0.5) and msg["speed_mps"] == pytest.approx(1.2, abs=0.02)
    now = base + int(times[order[-1]] * 1000)
    assert msg["eta_s"]["n04"] == pytest.approx(times["n04"] - (now - base) / 1000, abs=0.2)


def test_a_prearm_is_sent_again_only_when_its_window_runs_out():
    t = tracker(slow_speed_mps=0.5)
    t.detected("n01", T0, T0)
    sent = len(t.front.prearms["n03"])
    assert outputs(t.tick(T0 + 30_000), "prearm") == []  # still covered (60 s + 60 s window)
    t.s.slow_speed_mps = 0.1  # the fog now expected 5 times later: past every window sent
    out = t.tick(T0 + 40_000)
    assert [o[1] for o in outputs(out, "prearm")] == ["n02", "n03", "n04"] and len(t.front.prearms["n03"]) == sent + 1
    assert outputs(t.tick(T0 + 42_000), "prearm") == []  # covered again


def test_the_front_ends_and_the_site_learns_its_usual_front():
    t = tracker(min_fronts=3)
    for k in range(3):
        start = T0 + k * 1_000_000
        times = arrivals(90 + 5 * k, 1.0, t0=0)
        for node in sorted(["n01", "n02", "n03"], key=times.get):
            ts = start + int(times[node] * 1000)
            t.detected(node, ts, ts)
        for node in ("n01", "n02", "n03"):
            t.cleared(node, start + 90_000, start + 90_000)
        t.tick(start + 100_000)
        assert t.front is not None  # cleared everywhere, but n04 expected about 44 s in, + 60 s grace: not over
        out = t.tick(start + 110_000)
        (_, msg), = outputs(out, "front")
        assert valid(msg)["active"] is False and msg["eta_s"] == {} and t.front is None
    assert t.prior.fronts == 3 and t.prior.agreement > 0.99 and abs(t.prior.direction_deg - 95) < 1
    # the 4th front: one detection is enough to time the others, from the usual front
    out = t.detected("n01", T0 + 5_000_000, T0 + 5_000_000)
    (_, msg), = outputs(out, "front")
    assert msg["direction_deg"] == pytest.approx(95, abs=1) and set(msg["eta_s"]) == {"n02", "n03", "n04"}
    assert msg["eta_s"]["n03"] == pytest.approx(30 * math.sin(math.radians(95)), abs=0.5)


def test_fronts_from_everywhere_make_no_usual_front():
    p = Prior.toward(0, 1.0, source="learned", fronts=1)
    for d in (90, 200, 300, 45, 160):
        p.learn(d, 1.0, 0.3)
    assert p.agreement < 0.7 and p.slowness(0.7, 3) is None
    assert Prior.toward(90, 1.0).slowness(0.7, 3) == pytest.approx(slowness(90, 1.0))  # set by hand: trusted
    assert Prior.toward(90, 1.0, source="learned", fronts=2).slowness(0.7, 3) is None   # learned: 3 fronts first
    assert Prior.from_dict(p.as_dict()) == p


def test_detections_that_cannot_be_used():
    t = tracker()
    t.set_synced("n02", False)
    assert outputs(t.detected("n02", T0, T0), "log")[0][1].endswith("clock is not synced: not used for the front")
    t.places.pop("n03")
    t.set_state("n03", "ARMED")  # known, but no info message: no position
    assert "position is not known" in t.detected("n03", T0, T0)[0][1]
    assert "not a new front" in t.detected("n01", T0, T0 + 400_000)[0][1]  # replayed 400 s late
    assert t.front is None
    t.detected("n01", T0, T0)
    assert t.detected("n01", T0 + 50_000, T0 + 50_000) == []  # fog came back at n01: still the same front


def test_no_prearm_when_switched_off():
    t = tracker(prearm=False)
    assert outputs(t.detected("n01", T0, T0), "prearm") == []