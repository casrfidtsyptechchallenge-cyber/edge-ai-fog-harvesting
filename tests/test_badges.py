"""Tests for the station's badge list (M4): the rules, the file, the SYNC_ALLOWLIST command, and a round trip
through the simulated nodes of M0."""

import json

import pytest

import aeroion_contract as ac
from fake_node import CollectSink, Simulation
from station.badges import registry as reg
from station.badges.__main__ import main

T0 = 1791747040.0


def acks(sink):
    """(node_id, result, reason) of every CMD_ACK the simulated nodes sent."""
    out = []
    for topic, payload, _ in sink.messages:
        if topic.endswith("/event") and payload["type"] == "CMD_ACK":
            out.append((payload["node_id"], payload["data"]["result"], payload["data"]["reason"]))
    return out


# ---------------------------------------------------------------- UIDs

@pytest.mark.parametrize("text, uid", [
    ("04A1B2C3", "04A1B2C3"),
    ("04:a1:b2:c3", "04A1B2C3"),
    ("04 A1 B2 C3 D4 E5 F6", "04A1B2C3D4E5F6"),
    ("04-11-22-33-44-55-66-77-88-99", "04112233445566778899"),
])
def test_uids_are_written_the_contract_way(text, uid):
    assert reg.normalize_uid(text) == uid


@pytest.mark.parametrize("text", ["04A1B2", "04A1B2C3D", "04A1B2G3", "", "04A1B2C3" * 3, None])
def test_bad_uids_are_refused(text):
    with pytest.raises(reg.BadgeError):
        reg.normalize_uid(text)


# ---------------------------------------------------------------- changes and versions

def test_every_change_raises_the_version():
    r = reg.new_registry()
    assert reg.add(r, "04a1b2c3", "operator", "white card")
    assert reg.add(r, "04112233445566", "technician")
    assert r["version"] == 2
    assert reg.add(r, "04A1B2C3", "admin", "white card")  # change of role
    assert reg.find(r, "04A1B2C3")["role"] == "admin"
    assert reg.remove(r, "04112233445566")
    assert r["version"] == 4
    assert [b["uid"] for b in r["badges"]] == ["04A1B2C3"]


def test_no_change_keeps_the_version():
    r = reg.new_registry()
    reg.add(r, "04A1B2C3", "operator", "white card")
    assert not reg.add(r, "04 a1 b2 c3", "operator", "white card")
    assert not reg.remove(r, "04D4E5F6")
    assert r["version"] == 1


def test_unknown_roles_and_long_labels_are_refused():
    r = reg.new_registry()
    with pytest.raises(reg.BadgeError):
        reg.add(r, "04A1B2C3", "pilot")
    with pytest.raises(reg.BadgeError):
        reg.add(r, "04A1B2C3", "operator", "x" * 65)
    assert r == reg.new_registry()


def test_the_list_holds_the_contract_maximum():
    assert reg.MAX_BADGES == 64
    r = reg.new_registry()
    for i in range(reg.MAX_BADGES):
        reg.add(r, f"0400{i:04X}", "household")
    with pytest.raises(reg.BadgeError, match="full"):
        reg.add(r, "04FFFFFF", "operator")


# ---------------------------------------------------------------- the file

def test_save_and_load_give_back_the_same_list(tmp_path):
    path = tmp_path / "data" / "badges.json"
    r = reg.new_registry()
    reg.add(r, "04A1B2C3", "operator", "white card")
    reg.save(r, path)
    assert reg.load(path) == r
    assert not (tmp_path / "data" / "badges.json.tmp").exists()


def test_a_missing_file_is_an_empty_list(tmp_path):
    assert reg.load(tmp_path / "none.json") == {"version": 0, "badges": []}


def test_check_explains_what_is_wrong_in_a_hand_edited_file():
    problems = reg.check({"version": -1, "badges": [
        {"uid": "04a1b2c3", "role": "operator"},
        {"uid": "04D4E5F6", "role": "pilot"},
        {"uid": "04D4E5F6", "role": "operator", "colour": "red"},
    ]})
    text = "\n".join(problems)
    assert "version must be a whole number" in text
    assert "must be written '04A1B2C3'" in text
    assert "badge 1: role must be one of" in text
    assert "listed twice" in text
    assert "unknown key(s): colour" in text
    assert reg.check({"version": 0, "badges": []}) == []


def test_load_refuses_a_broken_file(tmp_path):
    path = tmp_path / "badges.json"
    path.write_text("{not json", encoding="utf-8")
    with pytest.raises(reg.BadgeError, match="not valid JSON"):
        reg.load(path)
    path.write_text('{"version": 1, "badges": [{"uid": "XYZ", "role": "operator"}]}', encoding="utf-8")
    with pytest.raises(reg.BadgeError, match="not a UID"):
        reg.load(path)


# ---------------------------------------------------------------- SYNC_ALLOWLIST

def test_the_sync_command_follows_the_contract():
    r = reg.new_registry()
    reg.add(r, "04A1B2C3", "operator", "white card")
    reg.add(r, "04112233445566", "technician")
    cmd = reg.sync_command(r, now=T0)
    assert ac.validate("cmd", cmd) == []
    assert cmd["type"] == "SYNC_ALLOWLIST"
    assert cmd["params"] == {"version": 2, "entries": [{"uid": "04A1B2C3", "role": "operator"},
                                                       {"uid": "04112233445566", "role": "technician"}]}
    assert "label" not in json.dumps(cmd)  # labels stay on the station


def test_the_simulated_nodes_take_the_list_and_refuse_an_older_one():
    sink = CollectSink()
    sim = Simulation(sink, nodes=2, t0=T0)
    sim.start()
    r = reg.new_registry()
    reg.add(r, "04A1B2C3", "operator")
    reg.add(r, "04D4E5F6", "technician")
    sim.send_command("all", reg.sync_command(r, now=sim.t))
    assert sorted(acks(sink)) == [("n01", "accepted", ""), ("n02", "accepted", "")]

    old = reg.new_registry()
    reg.add(old, "04A1B2C3", "operator")  # version 1 < 2
    sim.send_command("n01", reg.sync_command(old, now=sim.t))
    assert acks(sink)[-1][:2] == ("n01", "rejected")


# ---------------------------------------------------------------- command line

def test_command_line_add_list_remove_check(tmp_path, capsys):
    path = str(tmp_path / "badges.json")
    assert main(["--file", path, "add", "04:a1:b2:c3", "operator", "--label", "white card"]) == 0
    assert main(["--file", path, "add", "04112233445566", "technician"]) == 0
    assert main(["--file", path, "list"]) == 0
    out = capsys.readouterr().out
    assert "version 2, 2 badge(s)" in out and "04A1B2C3" in out and "white card" in out
    assert main(["--file", path, "remove", "04112233445566"]) == 0
    assert main(["--file", path, "remove", "04112233445566"]) == 1
    assert main(["--file", path, "check"]) == 0
    assert "OK: version 3, 1 badge(s)" in capsys.readouterr().out


def test_command_line_prints_one_contract_line(tmp_path, capsys):
    path = str(tmp_path / "badges.json")
    main(["--file", path, "add", "04A1B2C3", "operator"])
    capsys.readouterr()
    assert main(["--file", path, "sync", "--node", "n01", "--print"]) == 0
    line = json.loads(capsys.readouterr().out)
    assert line["topic"] == "aeroion/n01/cmd" and line["retain"] is False
    assert ac.validate("cmd", line["payload"]) == []


def test_command_line_reports_a_broken_file(tmp_path, capsys):
    path = tmp_path / "badges.json"
    path.write_text('{"version": 1, "badges": [{"uid": "04A1B2C3", "role": "pilot"}]}', encoding="utf-8")
    assert main(["--file", str(path), "check"]) == 2
    assert "role must be one of" in capsys.readouterr().err