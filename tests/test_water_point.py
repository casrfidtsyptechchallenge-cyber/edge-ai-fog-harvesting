"""Tests for the water point (M9, F13): sessions, quotas, the filter tag, the ledger, the service, the admin
commands. Every message it publishes is checked against the contract."""

import json
import time

import pytest

import aeroion_contract as ac
from station.badges import registry
from station.water import __main__ as cli
from station.water.hardware import SimHardware
from station.water.ledger import Ledger
from station.water.point import BadgeBook, PointSettings, WaterPoint, card_name, describe
from station.water.quality import QualityGate, Reading, Verdict
from station.water.service import EVENT_TOPIC, WATER_TOPIC, WaterService

T0 = 1791747040000
OK = Verdict("ok", "", 86.0, 6.9, 14.2)
BAD = Verdict("bad", "TDS 940 ppm, above 500", 940.0, 6.9, 14.2)
CARD7, CARD8, TECH, OPERATOR = "0A1B2C3D", "0B2C3D4E", "04112233445566", "04A1B2C3"
FILTER3, FILTER4, USED_UP = "04C3A1B2E35F80", "04C3A1B2E36080", "04C3A1B2E37A90"


class Cards:
    def __init__(self):
        self.badges = {CARD7: ("household", "card 7"), CARD8: ("household", ""), TECH: ("technician", "blue key fob"),
                       OPERATOR: ("operator", "white card")}

    def lookup(self, uid):
        return self.badges.get(uid)


def day_of(ms):  # UTC days: the tests do not depend on the computer's time zone
    return time.strftime("%Y-%m-%d", time.gmtime(ms / 1000))


@pytest.fixture
def ledger():
    led = Ledger()
    led.add_filter(FILTER3, "filter 3", 500.0, T0)
    led.add_filter(FILTER4, "filter 4", 500.0, T0)
    led.add_filter(USED_UP, "filter 2", 500.0, T0)
    led.set_filter_litres(USED_UP, 500.0)
    led.fit_filter(FILTER3, T0)
    return led


@pytest.fixture
def point(ledger):
    p = WaterPoint(ledger, Cards(), PointSettings(quota_l=20.0), day_of=day_of)
    p.set_quality(OK, T0)
    return p


def events(out, etype=None):
    found = [item[2] for item in out if item[0] == "event"]
    for msg in found:
        assert ac.validate("water_event", msg) == [], msg
    return [m["data"] for m in found if etype is None or m["type"] == etype]


def valid_status(p, now):
    st = p.status(now)
    assert ac.validate("water", st) == [], st
    return st


# ---------------------------------------------------------------- sessions

def test_a_household_draws_water_until_it_taps_again(point):
    point.tap(CARD7, T0)
    assert point.valve_open
    point.flow(1.5, T0 + 10_000)
    st = valid_status(point, T0 + 10_000)
    assert st["session"] == {"card": "card 7", "litres": 1.5, "quota_left_l": 18.5}
    assert st["dispensed_today_l"] == 1.5 and st["filter"]["litres"] == 1.5
    (d,) = events(point.tap(CARD7, T0 + 12_000), "DISPENSE")
    assert d == {"card": "card 7", "litres": 1.5, "quota_left_l": 18.5, "end": "card"}
    assert not point.valve_open
    assert point.ledger.used_on(day_of(T0), CARD7) == 1.5 and point.ledger.filter(FILTER3)["litres"] == 1.5


def test_the_valve_closes_at_the_quota_and_the_next_tap_is_refused(point):
    point.tap(CARD7, T0)
    for i in range(1, 21):
        out = point.flow(1.0, T0 + i * 6000)
    (d,) = events(out, "DISPENSE")
    assert (d["litres"], d["quota_left_l"], d["end"]) == (20.0, 0.0, "quota")
    (denied,) = events(point.tap(CARD7, T0 + 200_000), "CARD_DENIED")
    assert denied == {"card": "card 7", "reason": "daily quota used (20 L)"}
    assert not point.valve_open


def test_quotas_are_per_card_and_per_day(point):
    point.ledger.set_quota(CARD8, 2.0)
    point.tap(CARD8, T0)
    (d,) = events(point.flow(2.0, T0 + 1000), "DISPENSE")
    assert (d["card"], d["end"]) == ("…3D4E", "quota")  # no label: the end of its UID, never the whole UID
    assert events(point.tap(CARD8, T0 + 2000), "CARD_DENIED")
    point.tap(CARD7, T0 + 3000)
    assert point.valve_open  # card 7 still has its 20 L
    point.tap(CARD7, T0 + 4000)
    tomorrow = T0 + 24 * 3600 * 1000
    point.tap(CARD8, tomorrow)
    assert point.valve_open and point.session.quota_left == 2.0


@pytest.mark.parametrize("end, act", [
    ("idle", lambda p: p.tick(T0 + 1000 + 15_001)),                       # no flow for 15 s
    ("timeout", lambda p: [p.flow(0.01, T0 + i * 10_000) for i in range(1, 62)] and p.tick(T0 + 600_001)),
    ("quality", lambda p: p.set_quality(BAD, T0 + 2000)),
    ("stop", lambda p: p.stop(T0 + 2000)),
])
def test_every_reason_the_valve_closes(point, end, act):
    point.tap(CARD7, T0)
    point.flow(0.5, T0 + 1000)
    act(point)
    assert not point.valve_open
    (d,) = [e for e in events_in_ledger(point.ledger) if e["type"] == "DISPENSE"]
    assert d["data"]["end"] == end


def events_in_ledger(ledger):
    return ledger.events_on(day_of(T0))


def test_a_card_is_refused_when_the_water_is_not_safe(point):
    out = point.set_quality(BAD, T0 + 1000)
    (q,) = events(out, "QUALITY_CHANGE")
    assert q == {"verdict": "bad", "reason": "TDS 940 ppm, above 500", "tds_ppm": 940.0, "ph": 6.9}
    (denied,) = events(point.tap(CARD7, T0 + 2000), "CARD_DENIED")
    assert denied["reason"] == "water not safe: TDS 940 ppm, above 500"
    point.set_quality(Verdict("unknown", "no TDS reading"), T0 + 3000)
    (denied,) = events(point.tap(CARD7, T0 + 4000), "CARD_DENIED")
    assert denied["reason"] == "water quality unknown: no TDS reading"
    assert events(point.set_quality(Verdict("unknown", "no TDS reading"), T0 + 5000)) == []  # same verdict: no event


def test_other_cards_and_a_busy_water_point(point):
    point.tap(CARD7, T0)
    (busy,) = events(point.tap(CARD8, T0 + 1000), "CARD_DENIED")
    assert busy == {"card": "…3D4E", "reason": "busy: card 7 is drawing water"}
    (op,) = events(point.tap(OPERATOR, T0 + 2000), "CARD_DENIED")
    assert op == {"card": "white card", "reason": "operator badge: not a water card"}
    (unknown,) = events(point.tap("04DEADBEEF", T0 + 3000), "CARD_DENIED")
    assert unknown == {"card": "…BEEF", "reason": "unknown card"}
    assert point.valve_open and point.session.card == "card 7"


def test_drips_with_the_valve_closed_count_for_no_one(point):
    assert point.flow(0.05, T0) == [] and point.stray_l == 0.05
    assert point.ledger.total_on(day_of(T0)) == 0


# ---------------------------------------------------------------- the filter cartridge

def test_a_technician_fits_a_registered_filter(point):
    (denied,) = events(point.tap(FILTER4, T0), "CARD_DENIED")
    assert denied["reason"] == "filter tag: a technician's badge first"
    assert point.tap(TECH, T0 + 1000)[0] == ("log", "blue key fob (technician): tap the new filter's tag within 30 s")
    (used,) = events(point.tap(USED_UP, T0 + 2000), "CARD_DENIED")
    assert used == {"card": "filter 2", "reason": "used up (500 of 500 L): not fitted"}
    (change,) = events(point.tap(FILTER4, T0 + 3000), "FILTER_CHANGE")
    assert change == {"filter": "filter 4", "capacity_l": 500.0, "litres": 0.0}
    assert valid_status(point, T0 + 3000)["filter"] == {"tag": "filter 4", "state": "ok", "litres": 0.0,
                                                        "capacity_l": 500.0}
    assert point.ledger.fitted_filter()["uid"] == FILTER4
    assert events(point.tap(FILTER3, T0 + 4000), "CARD_DENIED")  # one badge, one filter


def test_the_technician_has_30_s(point):
    point.tap(TECH, T0)
    assert point.tick(T0 + 30_001) == [("log", "filter change: no filter tag tapped in time")]
    assert events(point.tap(FILTER4, T0 + 31_000), "CARD_DENIED")


def test_a_filter_that_runs_out_closes_the_valve_and_blocks_the_next_card(point):
    point.ledger.set_filter_litres(FILTER3, 485.0)  # 15 L left, less than the card's 20 L
    point.reload()
    assert valid_status(point, T0)["filter"]["state"] == "expiring"  # past 90 %
    point.tap(CARD7, T0)
    out = []
    for i in range(1, 60):
        out += point.flow(1.0, T0 + i * 6000)
        if not point.valve_open:
            break
    assert events(out, "DISPENSE")[0]["end"] == "filter"
    (expired,) = events(out, "FILTER_EXPIRED")
    assert expired == {"filter": "filter 3", "litres": 500.0, "capacity_l": 500.0}
    assert valid_status(point, T0 + 400_000)["filter"]["state"] == "expired"
    (denied,) = events(point.tap(CARD8, T0 + 400_000), "CARD_DENIED")
    assert denied["reason"] == "filter 3 used up: change it"


def test_no_filter_no_water(ledger):
    led = Ledger()
    p = WaterPoint(led, Cards(), day_of=day_of)
    p.set_quality(OK, T0)
    assert valid_status(p, T0)["filter"] == {"tag": None, "state": "none", "litres": None, "capacity_l": None}
    (denied,) = events(p.tap(CARD7, T0), "CARD_DENIED")
    assert denied["reason"] == "no filter fitted"


# ---------------------------------------------------------------- the ledger and the record

def test_the_ledger_survives_a_restart(tmp_path):
    path = tmp_path / "water.db"
    led = Ledger(path)
    led.add_filter(FILTER3, "filter 3", 500.0, T0)
    led.fit_filter(FILTER3, T0)
    p = WaterPoint(led, Cards(), day_of=day_of)
    p.set_quality(OK, T0)
    p.tap(CARD7, T0)
    p.flow(12.0, T0 + 5000)
    p.tap(CARD7, T0 + 6000)
    led.close()
    again = WaterPoint(Ledger(path), Cards(), day_of=day_of)  # restarted: the quota is not given again
    again.set_quality(OK, T0 + 10_000)
    again.tap(CARD7, T0 + 10_000)
    assert again.session.quota_left == 8.0 and again.filter["litres"] == 12.0
    assert again.ledger.report(day_of(T0)) == [(CARD7, "card 7", 12.0, 1)]


def test_card_names_never_show_the_whole_uid():
    assert card_name("04C3A1B2E35F80") == "…5F80"
    assert card_name("0A1B2C3D", "  card 7 ") == "card 7"
    assert len(card_name("0A1B2C3D", "x" * 64)) == 40


def test_the_badge_list_is_read_again_when_it_changes(tmp_path):
    path = tmp_path / "badges.json"
    reg = registry.new_registry()
    registry.add(reg, CARD7, "household", "card 7")
    registry.save(reg, path)
    book = BadgeBook(path)
    assert book.lookup(CARD7) == ("household", "card 7") and book.lookup(CARD8) is None
    registry.add(reg, CARD8, "household", "card 8")
    registry.save(reg, path)
    assert book.lookup(CARD8) == ("household", "card 8")
    assert book.uid_for_label("CARD 8") == CARD8
    path.write_text("{broken", encoding="utf-8")
    assert book.lookup(CARD8) == ("household", "card 8") and "not valid JSON" in book.error  # keeps the last good list


# ---------------------------------------------------------------- the service

class FakeLink:
    def __init__(self, connected=True):
        self.connected = connected
        self.on_connect = None
        self.sent = []

    def publish(self, topic, message, retain=False):
        if not self.connected:
            return False
        kind = topic.split("/")[-1]
        assert ac.validate(kind, message) == [], message
        self.sent.append((topic, json.loads(json.dumps(message)), retain))
        return True


def service(ledger, link):
    hw = SimHardware(flow_lpm=60.0, noise=False, clock=lambda: clock_s[0])
    p = WaterPoint(ledger, Cards(), day_of=day_of)
    return WaterService(p, QualityGate(), hw, link, log=lambda text: None), hw


clock_s = [0.0]


def run(svc, seconds, start_ms):
    """Runs the loop at 10 steps a second of simulated time."""
    for i in range(int(seconds * 10)):
        clock_s[0] = (start_ms + i * 100) / 1000
        svc.step(start_ms + i * 100)
    return start_ms + int(seconds * 1000)


def test_the_service_runs_the_valve_and_publishes(ledger):
    link = FakeLink()
    svc, hw = service(ledger, link)
    t = run(svc, 4, T0)                     # the probes settle: quality ok
    hw.tap(CARD7)
    t = run(svc, 3, t)                      # 60 L/min for 3 s
    assert hw.valve and svc.point.session.litres == pytest.approx(3.0, abs=0.15)
    hw.tap(CARD7)
    run(svc, 1, t)
    assert not hw.valve
    types = [m["type"] for topic, m, _ in link.sent if topic == EVENT_TOPIC]
    assert types == ["QUALITY_CHANGE", "DISPENSE"]
    statuses = [m for topic, m, retain in link.sent if topic == WATER_TOPIC and retain]
    flowing = [s for s in statuses if s["valve_open"]]
    assert len(flowing) >= 3  # every second while water flows
    assert ledger.unpublished() == []


def test_events_kept_while_the_broker_is_down_are_sent_later(ledger):
    link = FakeLink(connected=False)
    svc, hw = service(ledger, link)
    t = run(svc, 4, T0)
    hw.tap("04DEADBEEF")
    t = run(svc, 1, t)
    assert link.sent == [] and len(ledger.unpublished()) == 2
    link.connected = True
    link.on_connect()                       # the broker is back
    run(svc, 1, t)
    kept = [m for topic, m, _ in link.sent if topic == EVENT_TOPIC]
    assert [(m["type"], m["buffered"]) for m in kept] == [("QUALITY_CHANGE", True), ("CARD_DENIED", True)]
    assert ledger.unpublished() == []


def test_an_error_in_the_loop_closes_the_valve(ledger, monkeypatch):
    import station.water.service as service_module

    link = FakeLink()
    svc, hw = service(ledger, link)
    t = run(svc, 4, T0)
    hw.tap(CARD7)
    run(svc, 1, t)
    assert hw.valve

    def broken(now_ms):
        raise RuntimeError("SD card full")

    def one_turn_only(seconds):
        svc.running = False

    svc.step = broken
    svc.running = True
    monkeypatch.setattr(service_module.time, "sleep", one_turn_only)
    svc._loop()
    assert not hw.valve


def test_a_clean_stop_publishes_the_last_status(ledger):
    class StoppingLink(FakeLink):
        def start(self):
            pass

        def stop(self, last_status=None):
            self.last = last_status

    link = StoppingLink()
    svc, hw = service(ledger, link)
    t = run(svc, 4, T0)
    hw.tap(CARD7)
    run(svc, 1, t)
    svc.stop()
    assert not hw.valve
    assert ac.validate("water", link.last) == []
    assert link.last["quality"]["reason"] == "water point stopped" and link.last["valve_open"] is False
    assert [m["data"]["end"] for topic, m, _ in link.sent if topic == EVENT_TOPIC and m["type"] == "DISPENSE"] == ["stop"]


def test_describe_gives_one_line_per_event():
    for path in sorted((cli.REPO / "contract" / "examples").glob("water_event.*.json")):
        line = describe(json.loads(path.read_text(encoding="utf-8")))
        assert line and "\n" not in line


# ---------------------------------------------------------------- the admin commands

def test_admin_commands(tmp_path, capsys):
    db, badges = str(tmp_path / "water.db"), tmp_path / "badges.json"
    reg = registry.new_registry()
    registry.add(reg, CARD7, "household", "card 7")
    registry.add(reg, TECH, "technician", "blue key fob")
    registry.save(reg, badges)
    common = ["--db", db, "--badges", str(badges)]
    assert cli.main(["quota", "card 7", "25"] + common) == 0
    assert cli.main(["quotas"] + common) == 0
    assert "card 7" in capsys.readouterr().out and Ledger(db).quota(CARD7) == 25.0
    assert cli.main(["quota", "blue key fob", "5"] + common) == 2  # not a household card
    assert cli.main(["quota", "card 7", "--default"] + common) == 0 and Ledger(db).quota(CARD7) is None
    assert cli.main(["filter", "add", "04:c3:a1:b2:e3:5f:80", "--capacity", "500", "--label", "filter 3"] + common) == 0
    assert cli.main(["filter", "add", TECH, "--capacity", "500"] + common) == 2  # a badge, not a filter tag
    assert cli.main(["filter", "list"] + common) == 0
    assert "04C3A1B2E35F80" in capsys.readouterr().out
    led = Ledger(db)
    led.fit_filter(FILTER3, T0)
    p = WaterPoint(led, BadgeBook(badges), day_of=cli.local_day)
    now = int(time.time() * 1000)
    p.set_quality(OK, now)
    p.tap(CARD7, now)
    p.flow(3.25, now + 1000)
    p.tap(CARD7, now + 2000)
    p.tap("04DEADBEEF", now + 3000)
    led.close()
    capsys.readouterr()
    assert cli.main(["report"] + common) == 0
    out = capsys.readouterr().out
    assert "3.25 L" in out and "card 7" in out and "1 × unknown card" in out


def test_the_simulator_sets_itself_up(tmp_path, monkeypatch):
    monkeypatch.setattr(cli, "SIM_DIR", tmp_path / "sim")
    db, badges = cli.set_up_sim()
    led = Ledger(db)
    assert led.fitted_filter()["label"] == "filter 3" and led.quota("0B2C3D4E") == 2.0
    assert BadgeBook(badges).lookup("0A1B2C3D") == ("household", "card 7")
    led.close()
    assert cli.set_up_sim() == (db, badges)  # a second run changes nothing
    assert len(Ledger(db).filters()) == 3


def test_reading_without_probes_gives_nothing():
    hw = SimHardware(noise=False)
    hw.probes_in = False
    assert hw.read_quality(T0) == Reading(T0)