"""The live service (M8): the tracker on the broker, with the station's clock.

Listens to aeroion/<node>/info, status, telemetry and event; publishes aeroion/station/front (retained) and
the PREARM commands. The site's usual front is kept in a file, so it survives a restart.

If the service stops, the dashboard must not show a front forever: at start it publishes "no front", it
registers the same message as its MQTT last will, and it publishes it again on a clean stop.
"""

import json
import os
import threading
import time
import uuid
from pathlib import Path

import aeroion_contract as ac

from .tracker import Prior

REPO = Path(__file__).resolve().parents[2]
DEFAULT_PRIOR_FILE = REPO / "data" / "network_prior.json"  # data/ is not in the repository


def now_ms():
    return int(time.time() * 1000)


def no_front(ts_ms):
    return {"schema_version": ac.SCHEMA_VERSION, "ts": int(ts_ms), "active": False, "direction_deg": None,
            "speed_mps": None, "fit_quality": None, "nodes_detected": [], "eta_s": {}}


def load_prior(path):
    try:
        return Prior.from_dict(json.loads(Path(path).read_text(encoding="utf-8")))
    except (OSError, ValueError, KeyError, TypeError):
        return None


def save_prior(path, prior):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(prior.as_dict(), indent=2), encoding="utf-8")
    os.replace(tmp, path)  # never a half-written file


class NetworkService:
    def __init__(self, engine, host="localhost", port=1883, prior_file=None, log=print, clock=now_ms):
        import paho.mqtt.client as mqtt

        self.engine, self.host, self.port = engine, host, port
        self.prior_file = prior_file
        self.log, self.clock = log, clock
        self.lock = threading.Lock()
        self.running = False
        client_id = f"aeroion-network-{uuid.uuid4().hex[:6]}"
        if hasattr(mqtt, "CallbackAPIVersion"):
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        else:
            self.client = mqtt.Client(client_id=client_id)
        self.client.will_set(ac.station_topic("front"), json.dumps(no_front(clock())), qos=1, retain=True)
        self.client.on_connect = self._connected
        self.client.on_disconnect = lambda *a: self.log("broker lost: reconnecting")
        self.client.on_message = self._message
        self.client.reconnect_delay_set(1, 10)

    def _connected(self, client, userdata, flags, rc, *rest):
        if rc != 0:
            self.log(f"the broker refused the connection ({rc})")
            return
        for kind in ("info", "status", "telemetry", "event"):
            client.subscribe(f"{ac.TOPIC_ROOT}/+/{kind}", qos=1)
        self.log(f"connected to {self.host}:{self.port}")

    def _message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload)
        except (ValueError, UnicodeDecodeError):
            return
        with self.lock:
            actions = self.engine.handle(msg.topic, payload, self.clock())
        self.do(actions)

    def do(self, actions):
        for action in actions:
            if action[0] == "publish":
                _, topic, payload, retain = action
                self.client.publish(topic, json.dumps(payload, separators=(",", ":")), qos=1, retain=retain)
            elif action[0] == "log":
                self.log(action[1])
            elif action[0] == "learned" and self.prior_file:
                p = action[1]
                save_prior(self.prior_file, p)
                self.log(f"site's usual front: toward {p.direction_deg:.0f}° at {p.speed_mps:.2f} m/s "
                         f"({p.fronts} front(s), agreement {p.agreement:.2f})")

    def start(self):
        self.client.connect(self.host, self.port, 30)  # OSError if the broker cannot be reached
        self.client.loop_start()
        self.client.publish(ac.station_topic("front"), json.dumps(no_front(self.clock())), qos=1, retain=True)
        self.running = True

        def ticker():
            while self.running:
                time.sleep(1.0)
                with self.lock:
                    actions = self.engine.tick(self.clock())
                self.do(actions)

        threading.Thread(target=ticker, daemon=True, name="network-ticker").start()

    def stop(self):
        self.running = False
        self.client.publish(ac.station_topic("front"), json.dumps(no_front(self.clock())), qos=1,
                            retain=True).wait_for_publish(timeout=2)
        self.client.loop_stop()
        self.client.disconnect()