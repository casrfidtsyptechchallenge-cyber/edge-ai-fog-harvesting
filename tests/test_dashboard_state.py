"""Tests for station/dashboard/state.py (M10): what the dashboard shows, built from contract messages."""

import json
from pathlib import Path

import aeroion_contract as ac
from station.dashboard.state import StationState

EXAMPLES = Path(__file__).resolve().parents[1] / "contract" / "examples"
T0 = 1791747040000


def example(name):
    return json.loads((EXAMPLES / f"{name}.json").read_text(encoding="utf-8"))


def telemetry(node="n01", seq=1, ts=T0, **fields):
    t = example("telemetry.harvest")
    t.update(node_id=node, seq=seq, ts=ts, **fields)
    return t


def event(node, seq, etype, data, ts=T0, **extra):
    return {"schema_version": "1.1", "node_id": node, "ts": ts, "seq": seq, "type": etype, "data": data, **extra}


def node_of(st, node="n01", now=0):
    return next(n for n in st.summary(now)["nodes"] if n["node_id"] == node)


def test_every_contract_example_is_understood():
    st = StationState()
    for path in sorted(EXAMPLES.glob("*.json")):
        msg = json.loads(path.read_text(encoding="utf-8"))
        kind = path.name.split(".")[0]
        assert ac.validate(kind, msg) == [], path.name
        if kind in ac.STATION_KINDS:
            topic = ac.station_topic(kind)
        else:
            topic = "aeroion/all/cmd" if kind == "cmd" else ac.node_topic(msg["node_id"], kind)
        st.ingest(topic, msg, rx_ms=0)
    assert st.ignored == 0
    snap = st.snapshot(1000)
    assert [n["node_id"] for n in snap["nodes"]] == ["n01"]
    assert snap["front"]["direction_deg"] == 72.5
    assert snap["water"]["quality"]["verdict"] in ("ok", "bad")
    assert len(snap["feed"]) == 9 + 5 + 5  # every event, every command and every water event


def test_a_node_card_shows_the_latest_live_values():
    st = StationState()
    st.ingest("aeroion/n01/info", example("info.node"))
    st.ingest("aeroion/n01/status", example("status.online"))
    updates = st.ingest("aeroion/n01/telemetry", telemetry(), rx_ms=1000)
    assert updates == [("point", {"node": "n01", "p": [T0, 0.71, 0.66, 27.9, 4.83, 0.74, 0.62, 1.015],
                                  "buffered": False})]
    n = node_of(st, now=2000)
    assert (n["state"], n["live"], n["age_s"], n["mass_g"], n["hv_on"]) == ("HARVEST", True, 1.0, 4.83, True)
    assert n["health"]["score"] == 0.97 and n["position"] == {"x_m": 0.0, "y_m": 0.0}
    assert (n["fw"], n["time_synced"], n["rssi_dbm"], n["alerts"]) == ("0.1.0", True, -58.0, [])
    totals = st.summary(2000)["totals"]
    assert (totals["water_g"], totals["nodes_live"], totals["nodes_harvesting"]) == (4.83, 1, 1)
    assert totals["g_per_wh"] == round(4.83 / 0.0861, 1)


def test_the_card_shows_the_novelty_guard_and_tolerates_its_absence():
    st = StationState()
    st.ingest("aeroion/n01/telemetry", telemetry(arc_novel=0.35), rx_ms=1000)
    assert node_of(st, now=2000)["arc_novel"] == 0.35
    older = telemetry(seq=2, ts=T0 + 1000)
    older.pop("arc_novel", None)               # a node before contract 1.3: the field is optional
    st.ingest("aeroion/n01/telemetry", older, rx_ms=2000)
    assert node_of(st, now=3000)["arc_novel"] is None


def test_replayed_messages_are_history_not_live():
    st = StationState()
    st.ingest("aeroion/n01/telemetry", telemetry(seq=6, ts=T0 + 5000, mass_g=5.0), rx_ms=1000)
    updates = st.ingest("aeroion/n01/telemetry", telemetry(seq=1, ts=T0, mass_g=3.0, buffered=True), rx_ms=1100)
    assert updates[0][1]["buffered"] is True
    assert node_of(st, now=1200)["mass_g"] == 5.0  # the live value stays
    assert [p[0] for p in st.nodes["n01"].history] == [T0, T0 + 5000]  # the old one is back in its place
    feed = st.ingest("aeroion/n01/event", event("n01", 2, "FOG_CLEARED", {"duration_s": 184.0}, buffered=True))
    assert feed[0][1]["buffered"] is True


def test_a_message_received_twice_counts_once():
    st = StationState()
    warning = event("n01", 9, "ARC_PREDICTED", {"risk": 0.83, "hv_level": 0.74})
    assert st.ingest("aeroion/n01/event", warning)
    assert st.ingest("aeroion/n01/event", dict(warning, buffered=True)) == []  # the same seq and time
    assert st.ingest("aeroion/n01/telemetry", telemetry())
    assert st.ingest("aeroion/n01/telemetry", telemetry()) == []
    assert node_of(st)["counters"]["arc_warnings"] == 1 and len(st.feed) == 1


def test_events_feed_the_counters_and_the_event_list():
    st = StationState()
    events = [
        ("FOG_DETECTED", {"fog_idx": 0.32, "source": "optical"}, "info", "fog detected (0.32, optical)"),
        ("ARC", {"peak_ua": 1250.0, "hv_level": 0.78, "predicted": False}, "serious",
         "arc, peak 1250 µA at HV 0.78 (NOT predicted)"),
        ("ARC", {"peak_ua": 900.0, "hv_level": 0.7, "predicted": True}, "warning", "arc, peak 900 µA at HV 0.70 (predicted)"),
        ("STATE_CHANGE", {"from": "HARVEST", "to": "SAFE", "reason": "lid open"}, "critical", "HARVEST → SAFE (lid open)"),
        ("FAULT", {"code": "LID_OPEN", "detail": "lid opened"}, "critical", "fault LID_OPEN: lid opened"),
        ("BADGE_TAP", {"uid": "04A1B2C3", "role": "operator", "action": "arm", "result": "granted"}, "info",
         "badge 04A1B2C3 (operator): arm granted"),
        ("CMD_ACK", {"cmd_id": "dash-1", "result": "rejected", "reason": "state is LOCKED"}, "warning",
         "command dash-1 rejected: state is LOCKED"),
        ("SOMETHING_NEW", {}, "info", "event SOMETHING_NEW"),  # a newer node: shown, never a crash
    ]
    for seq, (etype, data, severity, words) in enumerate(events, 1):
        (name, entry), = st.ingest("aeroion/n02/event", event("n02", seq, etype, data))
        assert (name, entry["severity"], entry["text"], entry["node"]) == ("feed", severity, words, "n02")
    counters = node_of(st, "n02")["counters"]
    assert counters == {"fog_episodes": 1, "arc_warnings": 0, "arcs": 2, "arcs_missed": 1, "faults": 1}
    assert st.nodes["n02"].state_reason == "lid open"


def test_alerts_show_what_needs_attention_worst_first():
    st = StationState(stale_s=5)
    st.ingest("aeroion/n01/status", dict(example("status.online"), time_synced=False, buffered_msgs=3))
    st.ingest("aeroion/n01/event", event("n01", 1, "STATE_CHANGE", {"from": "HARVEST", "to": "SAFE", "reason": "lid"}))
    health = {"onset_level": 0.3, "k_coef": 100.0, "score": 0.3}
    st.ingest("aeroion/n01/telemetry", telemetry(seq=2, state="SAFE", radar_presence=True, lid_closed=False,
                                                 health=health, batt_pct=15.0), rx_ms=0)
    words = [(a["severity"], a["text"]) for a in node_of(st, now=1000)["alerts"]]
    assert words == [
        ("critical", "SAFE: HV forced off (lid)"),
        ("warning", "person near the collector: HV cut by the radar"),
        ("warning", "lid open"),
        ("warning", "electrode health 0.30: clean it soon"),
        ("warning", "clock not synced: ignored by the fog-front tracker"),
        ("warning", "battery 15 %"),
        ("info", "3 message(s) waiting in the node's offline buffer"),
    ]
    assert node_of(st, now=6001)["alerts"][1] == {"severity": "warning", "text": "no telemetry for 6 s"}
    st.ingest("aeroion/n01/status", example("status.last_will"))
    alerts = st.summary(6001)["alerts"]
    assert alerts[0] == {"severity": "critical", "text": "offline (the broker lost the node)", "node": "n01"}


def test_silence_is_judged_on_the_station_clock_not_the_node_clock():
    st = StationState(stale_s=5)
    st.ingest("aeroion/n01/telemetry", telemetry(ts=1767225600000), rx_ms=50_000)  # a node with no time yet
    assert node_of(st, now=52_000)["live"] is True
    assert node_of(st, now=56_000)["live"] is False


def test_the_charts_keep_the_last_minutes_only():
    st = StationState(history_s=60)
    for i in range(13):
        st.ingest("aeroion/n01/telemetry", telemetry(seq=i + 1, ts=T0 + i * 10_000))
    assert [p[0] - T0 for p in st.nodes["n01"].history] == [60_000, 70_000, 80_000, 90_000, 100_000, 110_000, 120_000]
    too_old = telemetry(seq=99, ts=T0, buffered=True)
    assert st.ingest("aeroion/n01/telemetry", too_old) == []


def test_foreign_and_broken_input_is_ignored():
    st = StationState()
    assert st.ingest("other/n01/telemetry", telemetry()) == []
    assert st.ingest("aeroion/n01/telemetry", "not json") == []
    assert st.ingest("aeroion/n01/telemetry", telemetry(schema_version="2.0")) == []  # another MAJOR version
    assert st.ingest("aeroion/n02/telemetry", telemetry(node="n01")) == []           # on another node's topic
    assert st.ingest("aeroion/sim/truth", {"anything": 1}) == []                     # simulator only, not counted
    assert st.ignored == 4
    st.ingest("aeroion/n01/telemetry", telemetry(mass_g=float("nan"), i_ua="12", hv_level=True, lid_closed="yes"))
    n = node_of(st)
    assert (n["mass_g"], n["i_ua"], n["hv_level"], n["lid_closed"]) == (None, None, None, None)
    long_reason = "x" * 500
    (_, entry), = st.ingest("aeroion/n01/event", event("n01", 5, "FAULT", {"code": "OTHER", "detail": long_reason}))
    assert len(entry["text"]) < 200


def test_station_commands_and_the_fog_front():
    st = StationState()
    (_, entry), = st.ingest("aeroion/all/cmd", example("cmd.lock_all"))
    assert (entry["node"], entry["text"]) == ("all", "command LOCK lock-0001 sent (emergency stop from dashboard)")
    (_, entry), = st.ingest("aeroion/n03/cmd", example("cmd.prearm"))
    assert entry["text"] == "command PREARM prearm-0007 sent (fog from n01 in 14 s)"
    (name, front), = st.ingest("aeroion/station/front", example("front.active"))
    assert name == "front" and front["eta_s"] == {"n03": 11.8} and front["nodes_detected"] == ["n01", "n02"]
    assert st.ingest("aeroion/station/front", dict(example("front.active"), active="yes")) == []


def test_a_new_browser_gets_everything():
    st = StationState(history_s=120)
    st.ingest("aeroion/n01/telemetry", telemetry())
    st.ingest("aeroion/n01/event", example("event.fog_detected"))
    snap = st.snapshot(1000)
    assert snap["history"]["n01"][0][0] == T0 and snap["history_s"] == 120
    assert snap["fields"][0] == "fog_idx_opt" and len(snap["feed"]) == 1 and snap["front"] is None
    json.dumps(snap)  # it goes to the browser as JSON




# ---------------------------------------------------------------- the water point (M9)

def test_the_water_point_status_and_its_alerts():
    st = StationState()
    assert st.summary(0)["water"] is None and st.ingest("aeroion/station/water", example("water.dispensing"), rx_ms=1000) == []
    w = st.summary(2000)["water"]
    assert (w["valve_open"], w["live"], w["age_s"], w["dispensed_today_l"]) == (True, True, 1.0, 41.7)
    assert w["session"] == {"card": "card 7", "litres": 4.2, "quota_left_l": 15.8}
    assert w["filter"] == {"tag": "filter 3", "state": "ok", "litres": 116.6, "capacity_l": 500.0}
    assert st.summary(2000)["alerts"] == []
    st.ingest("aeroion/station/water", example("water.bad"), rx_ms=3000)
    alerts = [(a["severity"], a["node"], a["text"]) for a in st.summary(3000)["alerts"]]
    assert alerts == [("serious", "water", "water not safe (TDS 940 ppm, above 500): the valve stays closed"),
                      ("warning", "water", "filter 3 nearly used up (462 of 500 L): change it soon")]
    silent = st.summary(3000 + 16_000)["alerts"]
    assert [a["text"] for a in silent] == ["water point silent for 16 s (last status: TDS 940 ppm, above 500)"]


def test_an_old_retained_water_status_is_shown_as_silent():
    st = StationState()
    old = dict(example("water.ok"), ts=T0)
    st.ingest("aeroion/station/water", old, retain=True, rx_ms=T0 + 60_000)  # the service stopped a minute ago
    assert st.summary(T0 + 60_000)["water"]["live"] is False
    st.ingest("aeroion/station/water", dict(old, ts=T0 + 61_000), rx_ms=T0 + 61_000)
    assert st.summary(T0 + 61_000)["water"]["live"] is True


def test_water_events_go_to_the_feed():
    st = StationState()
    words = {}
    for name in ("dispense", "card_denied", "quality_change", "filter_change", "filter_expired"):
        (kind, entry), = st.ingest("aeroion/station/water_event", example(f"water_event.{name}"))
        assert kind == "feed" and entry["node"] == "water"
        words[name] = (entry["severity"], entry["text"])
    assert words["dispense"] == ("info", "card 7 drew 20.0 L, 0.0 L left today (valve closed: quota reached)")
    assert words["card_denied"] == ("warning", "…B2C3: no water (daily quota used (20 L))")
    assert words["quality_change"] == ("serious", "water quality BAD: pH 5.6, below 6.5")
    assert words["filter_change"] == ("info", "filter 3 fitted (0 of 500 L used)")
    assert words["filter_expired"] == ("serious", "filter 3 used up (500 L): no water until it is changed")
    again = dict(example("water_event.dispense"), buffered=True)  # sent again after the broker came back
    assert st.ingest("aeroion/station/water_event", again) == []
    assert len(st.feed) == 5


def test_broken_water_messages_are_ignored():
    st = StationState()
    assert st.ingest("aeroion/station/water", dict(example("water.ok"), valve_open="yes")) == []
    assert st.ignored == 1 and st.water is None
    st.ingest("aeroion/station/water", dict(example("water.ok"), quality={"verdict": "great"}, filter=None))
    w = st.summary(0)["water"]
    assert w["quality"]["verdict"] == "unknown" and w["filter"]["state"] is None