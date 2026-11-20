"""Tests for the serial-to-MQTT bridge (M7, tools/serial_bridge.py): what it publishes, what it sends to the
board, and that the lines follow the link format of the MCX (docs/link.md)."""

import json

import pytest

import aeroion_contract as ac
from serial_bridge import LINE_MAX, Bridge
from station.badges import registry as reg

T0 = 1791747040.0


class Recorder:
    def __init__(self):
        self.published = []  # (topic, payload dict, qos, retain)
        self.board = []      # lines sent to the board
        self.shown = []      # lines shown to the person

    def bridge(self, node="n01", check=True):
        return Bridge(node, lambda t, text, q, r: self.published.append((t, json.loads(text), q, r)),
                      self.board.append, self.shown.append, check=check)


@pytest.fixture
def rec():
    return Recorder()


def board_line(topic, payload, retain=False):
    return json.dumps({"topic": topic, "retain": retain, "payload": payload}).encode() + b"\r"


STATUS = {"schema_version": ac.SCHEMA_VERSION, "node_id": "n01", "online": True, "ts": 1791747040000,
          "uptime_s": 12, "time_synced": True, "buffered_msgs": 0}
ACK = {"schema_version": ac.SCHEMA_VERSION, "node_id": "n01", "ts": 1791747040000, "seq": 3, "type": "CMD_ACK",
       "data": {"cmd_id": "c-1", "result": "accepted", "reason": ""}}


# ---------------------------------------------------------------- board -> MQTT

def test_contract_lines_are_published_with_their_retain_flag_and_qos(rec):
    b = rec.bridge()
    b.from_board(board_line("aeroion/n01/status", STATUS, retain=True))
    b.from_board(board_line("aeroion/n01/event", ACK))
    assert rec.published == [("aeroion/n01/status", STATUS, 1, True), ("aeroion/n01/event", ACK, 1, False)]
    assert rec.shown == []
    assert b.counts["status"] == 1 and b.counts["event"] == 1


def test_telemetry_goes_out_with_qos_0():
    with open(ac.CONTRACT_DIR / "examples" / "telemetry.harvest.json", encoding="utf-8") as f:
        telemetry = json.load(f)
    rec = Recorder()
    b = rec.bridge(node=telemetry["node_id"])
    b.from_board(board_line(ac.node_topic(telemetry["node_id"], "telemetry"), telemetry))
    assert rec.published[0][2:] == (0, False)


def test_lines_for_people_are_shown_not_published(rec):
    b = rec.bridge()
    b.from_board(b"# state ARMED (operator badge)\r\n")
    b.from_board(b"   ")
    b.from_board(b"\xff\xfe noise")
    assert rec.published == []
    assert rec.shown[0] == "# state ARMED (operator badge)"
    assert len(rec.shown) == 2


def test_broken_lines_from_the_board_are_not_published(rec):
    b = rec.bridge()
    b.from_board(b'{"topic":"aeroion/n01/status","payload":')          # cut short
    b.from_board(b'{"topic":"weather/today","payload":{}}')             # not a contract topic
    b.from_board(board_line("aeroion/n01/cmd", {"cmd_id": "x"}))        # a node does not send commands
    b.from_board(b'{"payload":{}}')                                     # no topic
    assert rec.published == []
    assert b.counts["not published"] == 4
    assert all(s.startswith("! not a contract line") for s in rec.shown)


def test_a_message_that_breaks_the_contract_is_published_with_a_warning(rec):
    b = rec.bridge()
    bad = dict(STATUS, uptime_s=-1)
    b.from_board(board_line("aeroion/n01/status", bad, retain=True))
    assert rec.published == [("aeroion/n01/status", bad, 1, True)]
    assert b.counts["invalid"] == 1
    assert "breaks the contract" in rec.shown[0]


def test_a_board_with_another_node_id_is_reported_once(rec):
    b = rec.bridge(node="n01")
    other = dict(STATUS, node_id="n02")
    b.from_board(board_line("aeroion/n02/status", other, retain=True))
    b.from_board(board_line("aeroion/n02/status", other, retain=True))
    assert len(rec.published) == 2
    assert len(rec.shown) == 1 and "--node n02" in rec.shown[0]


# ---------------------------------------------------------------- MQTT -> board

def test_a_command_goes_to_the_board_as_one_contract_line(rec):
    b = rec.bridge()
    registry = reg.new_registry()
    reg.add(registry, "04A1B2C3", "operator")
    cmd = reg.sync_command(registry, now=T0)
    b.from_broker("aeroion/all/cmd", json.dumps(cmd).encode())
    assert len(rec.board) == 1
    line = rec.board[0]
    assert "\n" not in line and len(line) <= LINE_MAX
    assert json.loads(line) == {"topic": "aeroion/all/cmd", "retain": False, "payload": cmd}
    assert rec.shown == [f"> SYNC_ALLOWLIST {cmd['cmd_id']} sent to the board"]


def test_the_largest_badge_list_fits_the_board_line(rec):
    b = rec.bridge()
    registry = reg.new_registry()
    for i in range(reg.MAX_BADGES):
        reg.add(registry, f"04A1B2C3D4E5F6A7{i:04X}", "technician")  # 10-byte UIDs: the longest
    b.from_broker("aeroion/n01/cmd", json.dumps(reg.sync_command(registry, now=T0)).encode())
    assert len(rec.board) == 1
    assert len(rec.board[0]) < LINE_MAX


def test_a_command_breaking_the_contract_still_reaches_the_board_so_it_can_answer(rec):
    b = rec.bridge()
    cmd = {"schema_version": "1.1", "cmd_id": "c-9", "ts": 1, "ttl_s": 30, "type": "REBOOT", "params": {}}
    b.from_broker("aeroion/n01/cmd", json.dumps(cmd))
    assert json.loads(rec.board[0])["payload"] == cmd
    assert "breaks the contract" in rec.shown[0]


def test_what_is_not_a_command_stays_here(rec):
    b = rec.bridge()
    b.from_broker("aeroion/n01/cmd", b"LOCK now")
    b.from_broker("aeroion/n01/cmd", b"[1, 2]")
    huge = {"schema_version": "1.1", "cmd_id": "c-big", "ts": 1, "ttl_s": 30, "type": "LOCK",
            "params": {"reason": "x" * LINE_MAX}}
    b.from_broker("aeroion/n01/cmd", json.dumps(huge))
    assert rec.board == []
    assert len(rec.shown) >= 3 and "not sent" in rec.shown[-1]


def test_typed_lines_go_to_the_board(rec):
    b = rec.bridge()
    b.typed("badges\r\n")
    b.typed("   \n")
    b.typed("allow 04A1B2C3 operator\n")
    assert rec.board == ["badges", "allow 04A1B2C3 operator"]


# ---------------------------------------------------------------- the link itself

def test_the_time_line_is_unix_milliseconds():
    line = json.loads(Bridge.time_line(now=T0 + 0.1234))
    assert line == {"link": "time", "ts": 1791747040123}


def test_subscriptions_and_last_will(rec):
    b = rec.bridge(node="n07")
    assert b.subscriptions() == ["aeroion/n07/cmd", "aeroion/all/cmd"]
    topic, text = b.last_will()
    assert topic == "aeroion/n07/status"
    will = json.loads(text)
    assert will["online"] is False and ac.validate("status", will) == []


def test_a_bad_node_id_is_refused():
    with pytest.raises(ValueError):
        Bridge("node1", print, print)