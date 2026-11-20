"""Tests for station/dashboard/server.py (M10): the page, the live stream, commands, password, replay."""

import base64
import http.client
import json
import subprocess
import sys
import threading
from pathlib import Path

import pytest

import aeroion_contract as ac
from station.dashboard import server
from station.dashboard.state import StationState

REPO = Path(__file__).resolve().parents[1]
EXAMPLES = REPO / "contract" / "examples"


def example(name):
    return json.loads((EXAMPLES / f"{name}.json").read_text(encoding="utf-8"))


class Running:
    """A dashboard on a free port, in a thread."""

    def __init__(self, password=None, sender=None):
        self.hub = server.Hub(StationState(), clock=lambda: 1000)
        self.sent = []
        if sender is not None:
            self.hub.send_command = sender(self.sent)
        self.httpd = server.make_server(self.hub, "127.0.0.1", 0, password)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        conn.request(method, path, body=body, headers=headers or {})
        r = conn.getresponse()
        return r.status, dict(r.getheaders()), r.read()

    def command(self, body, headers=None):
        h = {"Content-Type": "application/json", server.CSRF_HEADER: "1"}
        h.update(headers or {})
        return self.request("POST", "/api/cmd", json.dumps(body), h)

    def close(self):
        self.hub.close()
        self.httpd.shutdown()
        self.httpd.server_close()


def accepting(sent):
    return lambda topic, cmd: sent.append((topic, cmd)) or True


@pytest.fixture
def dash():
    d = Running(sender=accepting)
    yield d
    d.close()


def test_the_page_and_its_files_are_served(dash):
    status, headers, body = dash.request("GET", "/")
    assert status == 200 and headers["Content-Type"].startswith("text/html") and b"AeroIon-Edge" in body
    assert b"http" not in body.replace(b"http-equiv", b"")  # nothing from the internet: the hotspot has none
    status, headers, _ = dash.request("GET", "/static/app.js")
    assert status == 200 and headers["Content-Type"].startswith("text/javascript")
    for path in ("/static/../state.py", "/static/%2e%2e/state.py", "/static/nothing.js", "/api/nothing"):
        assert dash.request("GET", path)[0] == 404, path


def test_the_state_as_json(dash):
    dash.hub.ingest("aeroion/n01/telemetry", example("telemetry.harvest"))
    status, _, body = dash.request("GET", "/api/state")
    snap = json.loads(body)
    assert status == 200 and snap["nodes"][0]["node_id"] == "n01" and snap["commands"] is True
    assert snap["source"] == {"kind": "none", "connected": False, "detail": ""}


def read_event(resp):
    name = data = None
    while True:
        line = resp.readline().decode("utf-8").rstrip("\n")
        if line.startswith("event: "):
            name = line[7:]
        elif line.startswith("data: "):
            data = json.loads(line[6:])
        elif line == "" and name:
            return name, data


def test_the_stream_sends_a_snapshot_then_every_update(dash):
    conn = http.client.HTTPConnection("127.0.0.1", dash.port, timeout=5)
    conn.request("GET", "/api/stream")
    resp = conn.getresponse()
    assert resp.status == 200 and resp.getheader("Content-Type") == "text/event-stream"
    name, snap = read_event(resp)
    assert name == "snapshot" and snap["nodes"] == []
    dash.hub.ingest("aeroion/n01/event", example("event.fog_detected"))
    assert read_event(resp) == ("feed", {"ts": 1791747040265, "rx_ms": 1000, "node": "n01", "severity": "info",
                                         "text": "fog detected (0.32, optical)", "buffered": False})
    dash.hub.tick()
    name, tick = read_event(resp)
    assert name == "tick" and tick["nodes"][0]["counters"]["fog_episodes"] == 1
    conn.close()


def test_only_lock_and_tare_can_be_sent(dash):
    status, _, body = dash.command({"node": "n02", "type": "LOCK"}, {server.CSRF_HEADER: ""})
    assert status == 403  # not from the page: another site's form cannot send it
    status, _, body = dash.command({"node": "n02", "type": "LOCK", "reason": "test"})
    assert status == 200
    topic, cmd = dash.sent[-1]
    assert topic == "aeroion/n02/cmd" and json.loads(body)["cmd_id"] == cmd["cmd_id"]
    assert ac.validate("cmd", cmd) == [] and cmd["params"] == {"reason": "test"}
    assert dash.command({"node": "all", "type": "LOCK"})[0] == 200
    assert dash.sent[-1][0] == "aeroion/all/cmd" and dash.sent[-1][1]["params"]["reason"] == "stop from the dashboard"
    assert dash.command({"node": "n01", "type": "TARE"})[0] == 200 and dash.sent[-1][1]["params"] == {}
    for wrong in ({"node": "all", "type": "TARE"}, {"node": "n01", "type": "SET_HINT"},
                  {"node": "n01", "type": "PREARM"}, {"node": "x1", "type": "LOCK"}, ["LOCK"]):
        assert dash.command(wrong)[0] == 400, wrong
    assert dash.request("POST", "/api/cmd", "{not json", {server.CSRF_HEADER: "1"})[0] == 400
    assert len(dash.sent) == 3


def test_the_body_is_read_before_any_answer(dash):
    """An answer sent before the body has arrived is lost on Windows (the connection is reset): the server must
    wait for the whole body, even for a request it refuses."""
    import socket

    body = json.dumps({"node": "n01", "type": "LOCK"}).encode()
    with socket.create_connection(("127.0.0.1", dash.port), timeout=5) as s:
        s.sendall(b"POST /api/cmd HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                  b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body[:10])
        s.settimeout(0.3)
        with pytest.raises(socket.timeout):
            s.recv(100)  # no answer yet: the rest of the body has not come
        s.settimeout(5)
        s.sendall(body[10:])
        assert s.recv(100).startswith(b"HTTP/1.0 403")  # no page header: refused, after reading it all
    assert dash.sent == []


def test_commands_need_the_live_broker():
    replay = Running()
    try:
        assert replay.command({"node": "n01", "type": "LOCK"})[0] == 503
    finally:
        replay.close()
    down = Running(sender=lambda sent: lambda topic, cmd: False)
    try:
        assert down.command({"node": "n01", "type": "LOCK"})[0] == 502
    finally:
        down.close()


def test_a_password_protects_everything():
    d = Running(password="fog-2026", sender=accepting)
    try:
        status, headers, _ = d.request("GET", "/")
        assert status == 401 and "Basic" in headers["WWW-Authenticate"]

        def auth(pw):
            return {"Authorization": "Basic " + base64.b64encode(f"anyone:{pw}".encode()).decode()}

        assert d.request("GET", "/api/state", headers=auth("wrong"))[0] == 401
        assert d.request("GET", "/api/state", headers=auth("fog-2026"))[0] == 200
        assert d.command({"node": "n01", "type": "LOCK"})[0] == 401 and d.sent == []
        assert d.command({"node": "n01", "type": "LOCK"}, auth("fog-2026"))[0] == 200
    finally:
        d.close()


def test_a_recorded_run_plays_back(tmp_path):
    run = tmp_path / "run.jsonl"
    subprocess.run([sys.executable, str(REPO / "sim" / "fake_node.py"), "--out", str(run), "--no-wait",
                    "--duration", "40", "--seed", "1"], check=True, capture_output=True)
    hub = server.Hub(StationState(), clock=lambda: 0)
    pauses = []
    source = server.ReplaySource(hub, run, speed=4, sleep=pauses.append)
    source.run()
    snap = hub.snapshot()
    assert [n["node_id"] for n in snap["nodes"]] == ["n01", "n02", "n03"]
    assert all(len(points) >= 38 for points in snap["history"].values())
    assert snap["nodes"][0]["position"] == {"x_m": 0.0, "y_m": 0.0}
    assert pauses and max(pauses) <= server.ReplaySource.MAX_GAP_S and abs(sum(pauses) - 40 / 4) < 1.5
    assert snap["source"]["kind"] == "replay" and "end of the recording" in snap["source"]["detail"]


def test_a_browser_that_falls_behind_is_dropped():
    hub = server.Hub(StationState())
    q = hub.subscribe()
    for i in range(server.CLIENT_QUEUE + 1):
        hub.broadcast(("feed", i))
    assert q not in hub.clients and q.get_nowait() is None  # its stream ends; the browser reconnects


class FakeClient:
    def __init__(self, rc=0):
        self.subscribed, self.published, self.rc = [], [], rc

    def subscribe(self, topic, qos):
        self.subscribed.append(topic)

    def publish(self, topic, payload, qos):
        self.published.append((topic, json.loads(payload)))
        return type("Info", (), {"rc": self.rc})()


def test_the_broker_connection_is_shown_and_used():
    hub = server.Hub(StationState())
    source = server.MqttSource(hub, "pi.local", 1883)
    fake = FakeClient()
    source.client = fake
    source._connected(fake, None, {}, 0, None)
    assert fake.subscribed == ["aeroion/#"] and hub.snapshot()["source"]["connected"] is True
    source._disconnected(fake, None, {}, 7, None)
    assert hub.snapshot()["source"] == {"kind": "mqtt", "connected": False, "detail": "pi.local:1883 lost, reconnecting"}
    assert hub.send_command("aeroion/n01/cmd", {"type": "LOCK"}) is True and fake.published[0][0] == "aeroion/n01/cmd"
    fake.rc = 4
    assert hub.send_command("aeroion/n01/cmd", {"type": "LOCK"}) is False