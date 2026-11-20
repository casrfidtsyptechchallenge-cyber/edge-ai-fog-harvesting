"""Tests for tools/mcx_sim.py (M7 parts 2 and 3): the PC playing the MCX end of the ESP32's UART in Wokwi."""

import json

import pytest

import aeroion_contract as ac
from mcx_sim import Lines, Neighbour, SimulatedMcx, read_esp_line, relay_line

# The same packet as test_the_bytes_are_the_same_as_python in firmware/node/test/test_fog_alert.
ALERT_HEX = "AE0101077B23722CA101000001028001"


def cmd(cmd_id, ctype, params, ts=None):
    import time
    return {"schema_version": ac.SCHEMA_VERSION, "cmd_id": cmd_id, "ts": int((ts or time.time()) * 1000),
            "ttl_s": 30, "type": ctype, "params": params}


def test_lines_from_the_esp32_are_sorted():
    assert read_esp_line('{"link":"time","ts":1791747040123}', "n01") == ("time", 1791747040123)
    c = cmd("c-1", "LOCK", {"reason": "x"})
    line = json.dumps({"topic": "aeroion/all/cmd", "retain": False, "payload": c})
    assert read_esp_line(line, "n01") == ("cmd", ("aeroion/all/cmd", c))
    assert read_esp_line("# MQTT connected", "n01") == ("log", "# MQTT connected")
    assert read_esp_line('{"topic":"aeroion/n02/cmd","payload":{}}', "n01")[0] == "bad"
    assert read_esp_line('{"topic":', "n01")[0] == "bad"


def test_the_simulated_mcx_sends_contract_lines_and_answers_commands():
    sent = []
    mcx = SimulatedMcx("n01", sent.append, show=lambda _t: None, seed=1)
    mcx.start()
    kinds = [json.loads(line)["topic"] for line in sent]
    assert kinds == ["aeroion/n01/info", "aeroion/n01/status"]
    mcx.from_esp(json.dumps({"topic": "aeroion/n01/cmd", "retain": False,
                             "payload": cmd("c-lock", "LOCK", {"reason": "wokwi test"})}))
    mcx.step()
    lines = [json.loads(line) for line in sent]
    for line in lines:
        _, kind = ac.parse_topic(line["topic"])
        assert ac.validate(kind, line["payload"]) == []
    acks = [m["payload"]["data"] for m in lines if m["payload"].get("type") == "CMD_ACK"]
    assert acks == [{"cmd_id": "c-lock", "result": "accepted", "reason": ""}]
    assert any(m["topic"] == "aeroion/n01/telemetry" for m in lines)


def test_another_node_id_is_simulated_alone():
    sent = []
    mcx = SimulatedMcx("n03", sent.append, show=lambda _t: None, seed=1)
    mcx.start()
    assert {json.loads(line)["payload"]["node_id"] for line in sent} == {"n03"}


def test_only_json_lines_cross_in_relay_mode():
    assert relay_line('{"link":"time","ts":1}', "esp") == (True, None)
    assert relay_line("# station link up: the clock is synced", "mcx") == (False, "[mcx] # station link up: the clock is synced")
    assert relay_line("   ", "esp") == (False, None)


def test_bytes_are_cut_into_lines():
    lines = Lines()
    assert lines.feed(b'{"a":1}\r\n{"b"') == ['{"a":1}']
    assert lines.feed(b':2}\n') == ['{"b":2}']
    assert lines.feed(b"x" * 9000) == []
    assert lines.pending == b""


def test_fog_alert_bytes_are_the_same_as_the_firmware():
    assert ac.pack_fog_alert("n07", 1791747040123, 513, 0.5).hex().upper() == ALERT_HEX


def test_radio_lines_from_the_esp32_are_read():
    kind, alert = read_esp_line(json.dumps({"link": "radio", "hex": ALERT_HEX}), "n01")
    assert kind == "radio"
    assert alert == {"node_id": "n07", "ts_ms": 1791747040123, "seq": 513, "fog_idx": 128 / 255,
                     "time_synced": True, "virtual_source": False}
    assert read_esp_line('{"link":"radio","hex":"AE01"}', "n01")[0] == "bad"            # too short
    assert read_esp_line('{"link":"radio","hex":"zz"}', "n01")[0] == "bad"              # not hex
    assert read_esp_line('{"link":"radio","hex":"' + "00" * 16 + '"}', "n01")[0] == "bad"  # not a FOG_ALERT


def test_a_typed_alert_plays_a_neighbour():
    neighbour = Neighbour("n01")
    first = json.loads(neighbour.line_for("alert n02 0.6", 1791747040123))
    alert = ac.unpack_fog_alert(bytes.fromhex(first["hex"]))
    assert first["link"] == "radio"
    assert (alert["node_id"], alert["seq"], alert["fog_idx"]) == ("n02", 1, 153 / 255)
    assert (alert["ts_ms"], alert["time_synced"]) == (1791747040123, True)  # the ESP32's clock, as NTP gives it
    second = ac.unpack_fog_alert(bytes.fromhex(json.loads(neighbour.line_for("alert"))["hex"]))
    assert (second["node_id"], second["seq"]) == ("n02", 2)  # the default neighbour; each alert a new seq
    assert second["time_synced"] is False  # no time line from the ESP32 yet: no time either
    other = ac.unpack_fog_alert(bytes.fromhex(json.loads(Neighbour("n02").line_for("alert"))["hex"]))
    assert other["node_id"] == "n01"
    assert neighbour.line_for("status") is None
    for wrong in ("alert n01", "alert x9", "alert n03 high", "alert n03 1.5"):
        with pytest.raises(ValueError):
            neighbour.line_for(wrong)


def test_radio_lines_never_reach_the_real_mcx():
    line = json.dumps({"link": "radio", "hex": ALERT_HEX})
    forward, text = relay_line(line, "esp")
    assert forward is False and "FOG_ALERT broadcast by n07" in text and "fog 50 %" in text
    prearm = json.dumps({"topic": "aeroion/n01/cmd", "retain": False, "payload": {"type": "PREARM"}})
    assert relay_line(prearm, "esp") == (True, None)


def test_the_simulated_mcx_shows_the_radio():
    shown = []
    mcx = SimulatedMcx("n01", lambda _line: None, show=shown.append, seed=1)
    mcx.from_esp(json.dumps({"link": "radio", "hex": ALERT_HEX}))
    assert shown == ["[radio] FOG_ALERT broadcast by n07: fog 50 %, seq 513, seen 19:30:40 UTC"]



def test_the_simulated_mcx_judges_commands_by_the_esp32_clock():
    # Wokwi often runs slower than real time: its ESP32's clock falls behind the PC's. The real MCX takes its
    # time from the ESP32, so a command stamped with the ESP32's clock is fresh for it.
    sent = []
    mcx = SimulatedMcx("n01", sent.append, show=lambda _t: None, seed=1)
    mcx.start()
    esp_now = round(mcx.sim.t * 1000) - 100000  # 100 s behind the PC
    mcx.from_esp(json.dumps({"link": "time", "ts": esp_now}))
    lock = dict(cmd("c-lock", "LOCK", {"reason": "x"}), ts=esp_now)
    mcx.from_esp(json.dumps({"topic": "aeroion/n01/cmd", "retain": False, "payload": lock}))
    mcx.step()
    acks = [m["payload"]["data"] for m in map(json.loads, sent) if m["payload"].get("type") == "CMD_ACK"]
    assert acks == [{"cmd_id": "c-lock", "result": "accepted", "reason": ""}]