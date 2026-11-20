"""The water point running (M9): probes -> quality gate -> water point -> valve, ten times a second, and the
messages for the dashboard.

The valve never waits for the network: the loop runs whether the broker is there or not, and the ledger
keeps every event. Events that could not be published are sent again, with "buffered": true, once the
broker is back.

    aeroion/station/water         the status, retained: at once on a change, every second while water
                                  flows, every 5 s otherwise
    aeroion/station/water_event   DISPENSE, CARD_DENIED, QUALITY_CHANGE, FILTER_CHANGE, FILTER_EXPIRED

On a clean stop the last status says "water point stopped" with the valve closed. After a crash the
dashboard shows the water point as silent (no status for 15 s), and the valve, normally closed, shuts
when its driver pin is released.
"""

import json
import threading
import time
import traceback
import uuid

import aeroion_contract as ac

from .point import describe

WATER_TOPIC = ac.station_topic("water")
EVENT_TOPIC = ac.station_topic("water_event")


def now_ms():
    return int(time.time() * 1000)


class MqttLink:
    """The broker, connected in the background and again after every loss. publish() says if it went out."""

    def __init__(self, host="localhost", port=1883, log=print):
        import paho.mqtt.client as mqtt

        self.host, self.port, self.log = host, port, log
        self.connected = False
        self.stopping = False
        self.on_connect = None  # called (from paho's thread) after each connection
        client_id = f"aeroion-water-{uuid.uuid4().hex[:6]}"
        if hasattr(mqtt, "CallbackAPIVersion"):
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        else:
            self.client = mqtt.Client(client_id=client_id)
        self.client.on_connect = self._connected
        self.client.on_disconnect = self._disconnected
        self.client.reconnect_delay_set(1, 10)

    def _connected(self, client, userdata, flags, rc, *rest):
        if rc != 0:
            self.log(f"the broker refused the connection ({rc})")
            return
        self.connected = True
        self.log(f"broker {self.host}:{self.port} connected")
        if self.on_connect:
            self.on_connect()

    def _disconnected(self, *args):
        if self.connected and not self.stopping:
            self.log("broker lost: the ledger keeps the events, reconnecting")
        self.connected = False

    def start(self):
        self.client.connect_async(self.host, self.port, 30)
        self.client.loop_start()

    def publish(self, topic, message, retain=False):
        if not self.connected:
            return False
        info = self.client.publish(topic, json.dumps(message, separators=(",", ":"), ensure_ascii=False),
                                   qos=1, retain=retain)
        return info.rc == 0

    def stop(self, last_status=None):
        self.stopping = True
        if last_status is not None and self.connected:
            try:
                self.client.publish(WATER_TOPIC, json.dumps(last_status, separators=(",", ":"), ensure_ascii=False),
                                    qos=1, retain=True).wait_for_publish(timeout=2)
            except (RuntimeError, ValueError):
                pass
        self.client.loop_stop()
        self.client.disconnect()


class WaterService:
    STEP_S = 0.1
    SAMPLE_MS = 1000           # the probes, once a second
    STATUS_IDLE_MS = 5000
    STATUS_FLOWING_MS = 1000
    RELOAD_MS = 5000           # the fitted filter, in case the admin changed it

    def __init__(self, point, gate, hardware, link=None, log=print, clock=now_ms):
        self.point, self.gate, self.hw, self.link, self.log, self.clock = point, gate, hardware, link, log, clock
        self.lock = threading.Lock()
        self.running = False
        self.thread = None
        self.resend = True          # at start: whatever the ledger kept from the last run
        self.last_sample_ms = None
        self.last_status_ms = None
        self.last_key = None
        self.last_reload_ms = 0
        self.probe_error = None
        if link is not None:
            link.on_connect = self._broker_back

    def _broker_back(self):
        self.resend = True
        self.last_key = None        # and the status at once

    # ------------------------------------------------------------------ one turn of the loop

    def step(self, now_ms):
        with self.lock:
            out = self.point.flow(self.hw.take_flow(), now_ms)  # count what has passed before anything closes
            if self.last_sample_ms is None or now_ms - self.last_sample_ms >= self.SAMPLE_MS:
                self.last_sample_ms = now_ms
                self._sample(now_ms)
            out += self.point.set_quality(self.gate.evaluate(now_ms), now_ms)
            for uid in self.hw.take_taps():
                out += self.point.tap(uid, now_ms)
            out += self.point.tick(now_ms)
            self.hw.set_valve(self.point.valve_open)
            if now_ms - self.last_reload_ms >= self.RELOAD_MS:
                self.last_reload_ms = now_ms
                self.point.reload()
            for item in out:
                self._handle(item)
            self._status(now_ms)
            if self.resend:
                self._send_kept()

    def _sample(self, now_ms):
        try:
            self.gate.add(self.hw.read_quality(now_ms))
            if self.probe_error:
                self.log("probes answering again")
            self.probe_error = None
        except Exception as e:  # no reading: the verdict turns unknown after max_age_s, and the valve closes
            if self.probe_error != str(e):
                self.log(f"probes not readable: {e}")
            self.probe_error = str(e)

    def _handle(self, item):
        if item[0] == "log":
            self.log(item[1])
        elif item[0] == "event":
            _, event_id, msg = item
            self.log(describe(msg))
            if self.link is not None and self.link.publish(EVENT_TOPIC, msg):
                self.point.ledger.mark_published(event_id)

    def _status(self, now_ms):
        st = self.point.status(now_ms)
        q, s, f = st["quality"], st["session"], st["filter"]
        key = (q["verdict"], q["reason"], st["valve_open"], s and s["card"], f["tag"], f["state"])
        every = self.STATUS_FLOWING_MS if st["valve_open"] else self.STATUS_IDLE_MS
        if key != self.last_key or self.last_status_ms is None or now_ms - self.last_status_ms >= every:
            self.last_key, self.last_status_ms = key, now_ms
            if self.link is not None:
                self.link.publish(WATER_TOPIC, st, retain=True)

    def _send_kept(self):
        if self.link is None or not self.link.connected:
            return
        kept = self.point.ledger.unpublished()
        for event_id, msg in kept:
            if not self.link.publish(EVENT_TOPIC, dict(msg, buffered=True)):
                return  # lost again: the rest waits for the next connection
            self.point.ledger.mark_published(event_id)
        if kept:
            self.log(f"{len(kept)} event(s) kept by the ledger sent to the broker")
        self.resend = False

    # ------------------------------------------------------------------ running

    def snapshot(self):
        with self.lock:
            return self.point.status(self.clock())

    def start(self):
        if self.link is not None:
            self.link.start()
        self.running = True
        self.thread = threading.Thread(target=self._loop, daemon=True, name="water-point")
        self.thread.start()

    def _loop(self):
        while self.running:
            try:
                self.step(self.clock())
            except Exception:  # never leave the valve open on an error; try again in a second
                try:
                    self.hw.set_valve(False)
                finally:
                    self.log("water point error, valve closed:\n" + traceback.format_exc(limit=3))
                time.sleep(1.0)
            time.sleep(self.STEP_S)

    def stop(self):
        self.running = False
        if self.thread is not None:
            self.thread.join(timeout=2)
        now = self.clock()
        with self.lock:
            for item in self.point.stop(now):
                self._handle(item)
            self.hw.set_valve(False)
            last = self.point.status(now)
        last["quality"] = {"verdict": "unknown", "reason": "water point stopped", "tds_ppm": None, "ph": None,
                           "temp_c": None}
        self.hw.close()
        if self.link is not None:
            self.link.stop(last)
        self.point.ledger.close()