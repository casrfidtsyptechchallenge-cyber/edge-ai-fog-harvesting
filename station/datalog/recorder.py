"""Record every contract message from the MQTT broker into a run's raw.jsonl.

Each line is the message as received plus the station's receive time (rx_ts, ms), in the same
{"topic", "retain", "payload"} format as the simulator and serial_capture.py, so contract_check.py and
the dataset builder read all three the same way.
"""

import json
import time
from collections import Counter

import aeroion_contract as ac


class Recorder:
    """Writes messages to a file. Kept separate from MQTT so it can be tested without a broker."""

    FLUSH_EVERY_S = 1.0

    def __init__(self, raw_path):
        self.file = open(raw_path, "a", encoding="utf-8", newline="\n")
        self.counts = Counter()
        self.last_flush = time.monotonic()

    def handle(self, topic, payload_bytes, retain=False, rx_ms=None):
        try:
            payload = json.loads(payload_bytes)
        except (ValueError, UnicodeDecodeError):
            payload = payload_bytes.decode("utf-8", errors="replace")  # kept, and reported later as invalid
        line = {"topic": topic, "retain": bool(retain),
                "rx_ts": rx_ms if rx_ms is not None else int(time.time() * 1000), "payload": payload}
        self.file.write(json.dumps(line, separators=(",", ":"), ensure_ascii=False) + "\n")
        self.counts[topic.rsplit("/", 1)[-1]] += 1
        if time.monotonic() - self.last_flush >= self.FLUSH_EVERY_S:
            self.file.flush()
            self.last_flush = time.monotonic()

    def close(self):
        self.file.close()


def record_mqtt(raw_path, host="localhost", port=1883, duration_s=None, report_every_s=10, out=print):
    """Record until Ctrl-C or until duration_s has passed. Returns the message counts per kind."""
    import paho.mqtt.client as mqtt

    if hasattr(mqtt, "CallbackAPIVersion"):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="aeroion-recorder")
    else:
        client = mqtt.Client(client_id="aeroion-recorder")
    client.on_connect = lambda c, *a: c.subscribe(f"{ac.TOPIC_ROOT}/#", qos=1)  # again after a reconnect
    client.connect(host, port, 30)  # raises OSError if the broker is not reachable
    recorder = Recorder(raw_path)
    client.on_message = lambda c, u, msg: recorder.handle(msg.topic, msg.payload, msg.retain)
    client.loop_start()
    start = time.monotonic()
    next_report = start + report_every_s
    try:
        while duration_s is None or time.monotonic() - start < duration_s:
            time.sleep(0.2)
            if time.monotonic() >= next_report:
                next_report += report_every_s
                out("  " + ", ".join(f"{kind} {n}" for kind, n in sorted(recorder.counts.items())))
    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        client.disconnect()
        recorder.close()
    return dict(recorder.counts)