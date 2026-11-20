"""Tests for the M0 contract: schemas, examples, topics, FOG_ALERT and generated files."""

import copy
import json
import shutil
import subprocess
import sys

import pytest
from jsonschema import Draft202012Validator

import aeroion_contract as ac
from conftest import REPO

EXAMPLES = sorted((REPO / "contract" / "examples").glob("*.json"))


def load_example(name):
    return json.loads((REPO / "contract" / "examples" / f"{name}.json").read_text(encoding="utf-8"))


# ---------------------------------------------------------------- schemas and examples

@pytest.mark.parametrize("name", ["common", *ac.KINDS])
def test_schema_is_valid_json_schema(name):
    Draft202012Validator.check_schema(ac.load_schema(name))


def test_every_kind_has_an_example():
    kinds = {p.name.split(".")[0] for p in EXAMPLES}
    assert kinds == set(ac.KINDS)


@pytest.mark.parametrize("path", EXAMPLES, ids=lambda p: p.stem)
def test_example_is_valid(path):
    kind = path.name.split(".")[0]
    assert ac.validate(kind, json.loads(path.read_text(encoding="utf-8"))) == []


def test_every_event_type_and_cmd_type_has_a_data_shape():
    assert set(ac.SCHEMAS["event"]["$defs"]) == set(ac.EVENT_TYPES)
    assert set(ac.SCHEMAS["cmd"]["$defs"]) == set(ac.CMD_TYPES)
    assert set(ac.SCHEMAS["water_event"]["$defs"]) == set(ac.WATER_EVENT_TYPES)


# ---------------------------------------------------------------- the validator catches mistakes

def broken(name, change):
    msg = copy.deepcopy(load_example(name))
    change(msg)
    return name.split(".")[0], msg


@pytest.mark.parametrize("case", [
    ("typo in a field name", lambda m: m.update(rh_pc=m.pop("rh_pct"))),
    ("fog index above 1", lambda m: m.update(fog_idx_opt=1.4)),
    ("unknown state", lambda m: m.update(state="HARVST")),
    ("missing required field", lambda m: m.pop("e_wh")),
    ("negative current", lambda m: m.update(i_ua=-3)),
    ("bad node id", lambda m: m.update(node_id="node1")),
    ("timestamp in seconds instead of ms", lambda m: m.update(ts=1791747040)),
    ("extra field inside health", lambda m: m["health"].update(age=3)),
    ("schema major 2", lambda m: m.update(schema_version="2.0")),
], ids=lambda c: c[0])
def test_bad_telemetry_is_rejected(case):
    kind, msg = broken("telemetry.harvest", case[1])
    assert ac.validate(kind, msg) != []


def test_event_data_must_match_its_type():
    msg = load_example("event.fog_detected")
    msg["data"] = {"risk": 0.9, "hv_level": 0.7}          # ARC_PREDICTED data under FOG_DETECTED
    assert ac.validate("event", msg) != []


def test_cmd_params_must_match_its_type():
    msg = load_example("cmd.prearm")
    msg["params"] = {"hv_level": 0.5}
    assert ac.validate("cmd", msg) != []


def test_cmd_ttl_is_bounded():
    msg = load_example("cmd.lock_all")
    msg["ttl_s"] = 0
    assert ac.validate("cmd", msg) != []


@pytest.mark.parametrize("case", [
    ("valve open while the verdict is a word it does not know", lambda m: m["quality"].update(verdict="good")),
    ("pH above 14", lambda m: m["quality"].update(ph=15)),
    ("card label over 40 characters", lambda m: m["session"].update(card="household card 7, the blue one by the door")),
    ("negative litres", lambda m: m["session"].update(litres=-0.1)),
    ("filter without its tag", lambda m: m["filter"].pop("tag")),
    ("filter capacity 0", lambda m: m["filter"].update(capacity_l=0)),
    ("node_id on a station message", lambda m: m.update(node_id="n01")),
], ids=lambda c: c[0])
def test_bad_water_status_is_rejected(case):
    kind, msg = broken("water.dispensing", case[1])
    assert ac.validate(kind, msg) != []


def test_water_event_data_must_match_its_type():
    msg = load_example("water_event.dispense")
    msg["data"] = load_example("water_event.card_denied")["data"]  # CARD_DENIED data under DISPENSE
    assert ac.validate("water_event", msg) != []
    msg = load_example("water_event.dispense")
    msg["data"]["end"] = "broken"
    assert ac.validate("water_event", msg) != []
    assert ac.validate("water_event", dict(load_example("water_event.dispense"), buffered=True)) == []


def test_last_will_without_ts_is_valid():
    assert ac.validate("status", {"schema_version": "1.0", "node_id": "n05", "online": False}) == []


def test_minor_version_is_accepted():
    msg = load_example("status.last_will")
    msg["schema_version"] = "1.7"
    assert ac.validate("status", msg) == []


def test_unknown_kind_raises():
    with pytest.raises(ValueError):
        ac.validate("weather", {})


# ---------------------------------------------------------------- topics

def test_topic_round_trip():
    for kind in ac.NODE_KINDS:
        assert ac.parse_topic(ac.node_topic("n07", kind)) == ("n07", kind)
    for kind in ac.STATION_KINDS:
        assert ac.parse_topic(ac.station_topic(kind)) == ("station", kind)
    assert ac.station_topic("water") == "aeroion/station/water"
    assert ac.node_topic("all", "cmd") == "aeroion/all/cmd"
    assert ac.parse_topic("aeroion/all/cmd") == ("all", "cmd")


@pytest.mark.parametrize("topic", [
    "aeroion/n1/telemetry", "aeroion/n00/telemetry", "aeroion/n01/weather", "aeroion/all/telemetry",
    "aeroion/station/telemetry", "other/n01/telemetry", "aeroion/n01", "aeroion/n01/event/extra",
])
def test_bad_topics_raise(topic):
    with pytest.raises(ValueError):
        ac.parse_topic(topic)


def test_node_number_round_trip():
    assert ac.node_number("n42") == 42
    assert ac.node_id_from_number(7) == "n07"
    with pytest.raises(ValueError):
        ac.node_id_from_number(100)


# ---------------------------------------------------------------- FOG_ALERT

def test_fog_alert_is_16_bytes_and_round_trips():
    data = ac.pack_fog_alert("n07", 1791747040265, 513, 0.5, time_synced=True, virtual_source=True)
    assert len(data) == ac.FOG_ALERT_SIZE == 16
    back = ac.unpack_fog_alert(data)
    assert back["node_id"] == "n07"
    assert back["ts_ms"] == 1791747040265
    assert back["seq"] == 513
    assert abs(back["fog_idx"] - 0.5) <= 1 / 255
    assert back["time_synced"] and back["virtual_source"]


def test_fog_alert_layout_is_little_endian():
    data = ac.pack_fog_alert("n03", 0x0102030405060708, 0x0A0B, 1.0, time_synced=False)
    assert data[0] == 0xAE and data[1] == 1 and data[2] == 1 and data[3] == 3
    assert data[4:12] == bytes([8, 7, 6, 5, 4, 3, 2, 1])
    assert data[12:14] == bytes([0x0B, 0x0A])
    assert data[14] == 255 and data[15] == 0


def test_fog_alert_rejects_foreign_packets():
    data = bytearray(ac.pack_fog_alert("n01", 1791747040265, 1, 0.4))
    data[0] = 0x00
    with pytest.raises(ValueError):
        ac.unpack_fog_alert(bytes(data))
    with pytest.raises(ValueError):
        ac.unpack_fog_alert(b"\xae\x01")


# ---------------------------------------------------------------- generated files

def test_generated_files_are_up_to_date():
    result = subprocess.run([sys.executable, str(REPO / "contract" / "generate.py"), "--check"],
                            capture_output=True, text=True, cwd=REPO)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("compiler,std,lang", [("gcc", "-std=c11", "c"), ("g++", "-std=c++17", "c++")])
def test_header_compiles(tmp_path, compiler, std, lang):
    if shutil.which(compiler) is None:
        pytest.skip(f"{compiler} not installed")
    src = tmp_path / "main.c"
    src.write_text('#include "aeroion_contract.h"\n'
                   "int main(void) { return aeroion_state_name(AEROION_STATE_ARMED)[0] == 'A' ? 0 : 1; }\n")
    exe = tmp_path / "main"
    build = subprocess.run([compiler, "-Wall", "-Wextra", "-Werror", std, "-x", lang,
                            "-I", str(REPO / "firmware" / "node" / "include"), str(src), "-o", str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    assert subprocess.run([str(exe)]).returncode == 0
