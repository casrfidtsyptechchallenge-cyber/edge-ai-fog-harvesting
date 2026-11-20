"""The dashboard's server (M10): the page, a live stream to the browsers, and two safe commands.

Python's standard library only (plus paho-mqtt for the broker), so it runs on the Pi with nothing more to
install and needs no internet: every file the page uses is in static/.

    GET  /                the page (static/index.html)
    GET  /static/<file>   its script and style
    GET  /api/state       everything, as JSON
    GET  /api/stream      Server-Sent Events: "snapshot" once, then "tick" every second, "point", "feed", "front"
    POST /api/cmd         {"node": "n01" or "all", "type": "LOCK" or "TARE", "reason": "..."}

Only LOCK (stop) and TARE (zero the load cell) can be sent from the page: neither can switch the HV on.
"""

import base64
import hmac
import json
import queue
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

import aeroion_contract as ac

STATIC_DIR = Path(__file__).resolve().parent / "static"
CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml"}
CSRF_HEADER = "X-AeroIon-Dashboard"  # the page sends it; a form on another site cannot
PAGE_COMMANDS = ("LOCK", "TARE")
CLIENT_QUEUE = 500                   # updates waiting for one browser; a browser that falls behind is dropped
MAX_BODY = 2048                      # a command is a few dozen bytes

def now_ms():
    return int(time.time() * 1000)


def build_command(ctype, params, ttl_s=30):
    return {"schema_version": ac.SCHEMA_VERSION, "cmd_id": "dash-" + uuid.uuid4().hex[:8], "ts": now_ms(),
            "ttl_s": ttl_s, "type": ctype, "params": params}


class Hub:
    """Joins the message source, the state and the browsers. Every method is safe from any thread."""

    def __init__(self, state, clock=now_ms):
        self.state = state
        self.clock = clock
        self.lock = threading.Lock()
        self.clients = set()
        self.source = {"kind": "none", "connected": False, "detail": ""}
        self.send_command = None  # set by a source that can publish: send_command(topic, cmd) -> bool
        self.closed = False

    def ingest(self, topic, payload, retain=False):
        with self.lock:
            updates = self.state.ingest(topic, payload, retain, self.clock())
        for update in updates:
            self.broadcast(update)

    def set_source(self, kind, connected, detail=""):
        with self.lock:
            self.source = {"kind": kind, "connected": connected, "detail": detail}

    def snapshot(self):
        with self.lock:
            out = self.state.snapshot(self.clock())
            out["source"] = dict(self.source)
        out["commands"] = self.send_command is not None
        return out

    def tick(self):
        with self.lock:
            out = self.state.summary(self.clock())
            out["source"] = dict(self.source)
        self.broadcast(("tick", out))

    def subscribe(self):
        q = queue.Queue(maxsize=CLIENT_QUEUE)
        with self.lock:
            self.clients.add(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.clients.discard(q)

    def broadcast(self, update):
        with self.lock:
            clients = list(self.clients)
        for q in clients:
            try:
                q.put_nowait(update)
            except queue.Full:  # too slow (a sleeping tablet): end its stream; it reconnects with a fresh snapshot
                self.unsubscribe(q)
                with q.mutex:
                    q.queue.clear()
                q.put_nowait(None)

    def run_ticker(self, every_s=1.0):
        def loop():
            while not self.closed:
                time.sleep(every_s)
                self.tick()
        threading.Thread(target=loop, daemon=True, name="dashboard-ticker").start()

    def close(self):
        self.closed = True
        self.broadcast(None)


def decode(payload_bytes):
    try:
        return json.loads(payload_bytes)
    except (ValueError, UnicodeDecodeError):
        return None  # counted as ignored by the state


class MqttSource:
    """Live messages from the broker (aeroion/#), and the way out for the page's commands."""

    def __init__(self, hub, host="localhost", port=1883):
        import paho.mqtt.client as mqtt

        self.hub, self.host, self.port = hub, host, port
        if hasattr(mqtt, "CallbackAPIVersion"):
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"aeroion-dashboard-{uuid.uuid4().hex[:6]}")
        else:
            self.client = mqtt.Client(client_id=f"aeroion-dashboard-{uuid.uuid4().hex[:6]}")
        self.client.on_connect = self._connected
        self.client.on_disconnect = self._disconnected
        self.client.on_message = lambda c, u, msg: hub.ingest(msg.topic, decode(msg.payload), msg.retain)
        self.client.reconnect_delay_set(1, 10)
        hub.send_command = self.send

    def _connected(self, client, userdata, flags, rc, *rest):
        if rc == 0:
            client.subscribe(f"{ac.TOPIC_ROOT}/#", qos=1)  # again after every reconnect
            self.hub.set_source("mqtt", True, f"{self.host}:{self.port}")
        else:
            self.hub.set_source("mqtt", False, f"{self.host}:{self.port} refused the connection ({rc})")

    def _disconnected(self, *args):
        self.hub.set_source("mqtt", False, f"{self.host}:{self.port} lost, reconnecting")

    def start(self):
        self.hub.set_source("mqtt", False, f"connecting to {self.host}:{self.port}")
        self.client.connect_async(self.host, self.port, 30)
        self.client.loop_start()

    def send(self, topic, cmd):
        info = self.client.publish(topic, json.dumps(cmd, separators=(",", ":")), qos=1)
        return info.rc == 0

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()


class ReplaySource:
    """A recorded run (sim/fake_node.py --out, or a station.datalog raw.jsonl) played back at its own pace.

    speed 4 plays four times faster; speed 0 loads everything at once. Gaps longer than 5 s are shortened.
    """

    MAX_GAP_S = 5.0

    def __init__(self, hub, path, speed=1.0, sleep=time.sleep):
        self.hub, self.path, self.speed, self.sleep = hub, Path(path), speed, sleep
        self.played = 0

    def lines(self):
        data = self.path.read_bytes()
        text = data.decode("utf-16") if data.startswith((b"\xff\xfe", b"\xfe\xff")) else data.decode("utf-8-sig")
        return text.splitlines()

    def run(self):
        self.hub.set_source("replay", True, f"{self.path.name}, speed {self.speed:g}")
        last_ts = None
        for line in self.lines():
            line = line.strip().lstrip("\ufeff")
            if not line.startswith("{"):
                continue
            try:
                row = json.loads(line)
                topic, payload = row["topic"], row["payload"]
            except (ValueError, KeyError, TypeError):
                continue
            ts = payload.get("ts") if isinstance(payload, dict) else None
            if self.speed > 0 and isinstance(ts, int) and last_ts is not None and ts > last_ts:
                self.sleep(min((ts - last_ts) / 1000 / self.speed, self.MAX_GAP_S))
            if isinstance(ts, int):
                last_ts = ts if last_ts is None else max(last_ts, ts)
            self.hub.ingest(topic, payload, bool(row.get("retain")))
            self.played += 1
        self.hub.set_source("replay", False, f"{self.path.name}: end of the recording ({self.played} messages)")

    def start(self):
        threading.Thread(target=self.run, daemon=True, name="dashboard-replay").start()


def make_handler(hub, password=None):
    static_files = {p.name: p for p in STATIC_DIR.iterdir() if p.suffix in CONTENT_TYPES}

    class Handler(BaseHTTPRequestHandler):
        server_version = "AeroIonDashboard/1.0"

        def log_message(self, fmt, *args):
            pass  # one line per request would flood the Pi's terminal

        # ---------------------------------------------------------- helpers

        def _authorised(self):
            if not password:
                return True
            header = self.headers.get("Authorization", "")
            if header.startswith("Basic "):
                try:
                    _, _, given = base64.b64decode(header[6:]).decode("utf-8").partition(":")
                except (ValueError, UnicodeDecodeError):
                    given = ""
                if hmac.compare_digest(given.encode("utf-8"), password.encode("utf-8")):
                    return True
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="AeroIon station", charset="UTF-8"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return False

        def _send(self, code, body, content_type):
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, code, obj):
            self._send(code, json.dumps(obj, separators=(",", ":")).encode("utf-8"), "application/json")

        def _event(self, name, data):
            self.wfile.write(f"event: {name}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n".encode("utf-8"))
            self.wfile.flush()

        # ---------------------------------------------------------- GET

        def do_GET(self):
            if not self._authorised():
                return
            path = urlsplit(self.path).path
            if path in ("/", "/index.html"):
                path = "/static/index.html"
            if path.startswith("/static/") and path[len("/static/"):] in static_files:
                f = static_files[path[len("/static/"):]]
                return self._send(200, f.read_bytes(), CONTENT_TYPES[f.suffix])
            if path == "/api/state":
                return self._json(200, hub.snapshot())
            if path == "/api/stream":
                return self._stream()
            self._json(404, {"error": "not found"})

        def _stream(self):
            q = hub.subscribe()  # before the snapshot, so nothing falls between the two
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self._event("snapshot", hub.snapshot())
                while not hub.closed:
                    try:
                        update = q.get(timeout=15)
                    except queue.Empty:
                        self.wfile.write(b": still here\n\n")  # keeps proxies and sleeping Wi-Fi from closing it
                        self.wfile.flush()
                        continue
                    if update is None:
                        break
                    self._event(*update)
            except OSError:
                pass  # the browser went away
            finally:
                hub.unsubscribe(q)

        # ---------------------------------------------------------- POST

        def _read_body(self):
            """The whole request body, before any answer: a server that answers and closes with the body still
            unread makes Windows reset the connection, and the browser may lose the answer."""
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = 0
            if length > MAX_BODY:
                self.close_connection = True
                return None  # too big to be a command: not read, refused
            return self.rfile.read(length) if length > 0 else b""

        def do_POST(self):
            raw = self._read_body()
            if not self._authorised():
                return
            if urlsplit(self.path).path != "/api/cmd":
                return self._json(404, {"error": "not found"})
            if self.headers.get(CSRF_HEADER) != "1":
                return self._json(403, {"error": "commands come from the dashboard page only"})
            try:
                body = json.loads(raw) if raw else None
            except (ValueError, UnicodeDecodeError):
                body = None
            if not isinstance(body, dict):
                return self._json(400, {"error": "send a JSON object"})
            node, ctype = body.get("node"), body.get("type")
            if ctype not in PAGE_COMMANDS:
                return self._json(400, {"error": "only LOCK and TARE can be sent from the dashboard"})
            if not (ac.is_valid_node_id(node) or (node == ac.BROADCAST and ctype == "LOCK")):
                return self._json(400, {"error": "node must be a node id (or 'all' for LOCK)"})
            reason = body.get("reason") if isinstance(body.get("reason"), str) else ""
            params = {"reason": reason.strip()[:60] or "stop from the dashboard"} if ctype == "LOCK" else {}
            cmd = build_command(ctype, params)
            errors = ac.validate("cmd", cmd)
            if errors:
                return self._json(500, {"error": errors[0]})
            sender = hub.send_command
            if sender is None:
                return self._json(503, {"error": "commands need the live broker (this is a replay)"})
            topic = ac.node_topic(node, "cmd")
            if not sender(topic, cmd):
                return self._json(502, {"error": "the broker is not reachable: command not sent"})
            self._json(200, {"cmd_id": cmd["cmd_id"], "topic": topic})

    return Handler


def make_server(hub, host="127.0.0.1", port=8080, password=None):
    server = ThreadingHTTPServer((host, port), make_handler(hub, password))
    server.daemon_threads = True  # open streams do not keep the program alive
    return server