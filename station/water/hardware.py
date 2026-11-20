"""The water point's hardware: simulated (any computer), or the real parts on the Raspberry Pi.

Both give the service the same five things:

    read_quality(now_ms) -> Reading     TDS (ppm, at 25 °C), pH and water temperature
    take_flow() -> litres               counted by the flow meter since the last call
    take_taps() -> [uid, ...]           cards and tags held to the reader since the last call
    set_valve(open)                     the outlet valve
    close()                             valve shut, parts released

The Pi parts (PiHardware) follow their datasheets but have NOT been run on the real board yet: check each one
on the bench first (python -m station.water probe), as docs/water_point.md explains.
"""

import glob
import json
import queue
import random
import threading
import time
from dataclasses import asdict, dataclass, fields
from pathlib import Path

from .quality import Reading

# ---------------------------------------------------------------------------------------- simulated


class SimHardware:
    """A tank, a valve, a flow meter, two probes and a reader, driven from the keyboard (see __main__)."""

    def __init__(self, flow_lpm=10.0, clock=time.monotonic, noise=True):
        self.tds_ppm, self.ph, self.temp_c = 86.0, 6.9, 14.0
        self.probes_in = True
        self.flow_lpm = flow_lpm          # while the valve is open; 0 = the tank is empty
        self.valve = False
        self.clock, self.noise = clock, noise
        self._taps = queue.Queue()
        self._lock = threading.Lock()
        self._last = clock()
        self._litres = 0.0

    def tap(self, uid):
        self._taps.put(uid)

    def read_quality(self, now_ms):
        if not self.probes_in:
            return Reading(now_ms)
        jitter = (lambda a: random.uniform(-a, a)) if self.noise else (lambda a: 0.0)
        return Reading(now_ms, max(0.0, self.tds_ppm + jitter(2)), self.ph + jitter(0.03), self.temp_c + jitter(0.1))

    def _run(self):
        now = self.clock()
        if self.valve:
            self._litres += self.flow_lpm / 60 * (now - self._last)
        self._last = now

    def take_flow(self):
        with self._lock:
            self._run()
            litres, self._litres = self._litres, 0.0
        return litres

    def take_taps(self):
        out = []
        while True:
            try:
                out.append(self._taps.get_nowait())
            except queue.Empty:
                return out

    def set_valve(self, open_):
        with self._lock:
            self._run()
            self.valve = bool(open_)

    def close(self):
        self.set_valve(False)


# ---------------------------------------------------------------------------------------- Raspberry Pi

@dataclass
class PiConfig:
    """data/water_point.json. Pins are BCM numbers (GPIO17 is header pin 11)."""
    valve_pin: int = 17              # relay or MOSFET driving the normally-closed 12 V solenoid valve
    valve_active_high: bool = True   # keep True: the valve must stay shut while the Pi boots or is off
    flow_pin: int = 27               # YF-S201 pulse output, through a 10 k / 20 k divider (it swings to 5 V)
    pulses_per_l: float = 450.0      # YF-S201: f = 7.5 × Q (L/min); calibrate with a measuring jug
    i2c_bus: int = 1
    ads_address: int = 0x48          # ADS1115, ADDR pin to GND
    tds_channel: int = 0             # Gravity TDS meter analog output on A0
    ph_channel: int = 1              # Gravity pH meter V2 analog output on A1
    ph7_v: float = 1.500             # probe voltage in the pH 7.00 buffer (calibrate: probe command)
    ph4_v: float = 2.032             # probe voltage in the pH 4.00 buffer
    tds_k: float = 1.0               # TDS calibration factor (from a known solution)
    reader: str = "rc522"            # "rc522", or "none" to type the taps in the terminal
    rc522_rst_pin: int = 25          # BCM 25 = header pin 22, the usual RC522 wiring

    @classmethod
    def load(cls, path):
        path = Path(path)
        if not path.exists():
            return cls()
        data = json.loads(path.read_text(encoding="utf-8"))
        known = {f.name for f in fields(cls)}
        unknown = set(data) - known
        if unknown:
            raise ValueError(f"{path}: unknown setting(s) {', '.join(sorted(unknown))}")
        return cls(**data)

    def save(self, path):
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_text(json.dumps(asdict(self), indent=2) + "\n", encoding="utf-8")


def tds_from_volts(volts, temp_c=25.0, k=1.0):
    """DFRobot Gravity TDS meter: the probe voltage, brought back to 25 °C (2 % per °C), to ppm."""
    v = volts / (1.0 + 0.02 * (temp_c - 25.0))
    return max(0.0, (133.42 * v ** 3 - 255.86 * v ** 2 + 857.39 * v) * 0.5 * k)


def ph_from_volts(volts, ph7_v, ph4_v):
    """Two-point calibration: the straight line through (ph7_v, 7.00) and (ph4_v, 4.00)."""
    return 7.0 + (volts - ph7_v) * (7.0 - 4.0) / (ph7_v - ph4_v)


class Ads1115:
    """The 16-bit ADC on I2C, single-shot, ±4.096 V range (power it from 3.3 V)."""

    def __init__(self, bus=1, address=0x48):
        from smbus2 import SMBus

        self.bus, self.address = SMBus(bus), address

    def volts(self, channel):
        # OS=1 (start), MUX=1xx (AINx against GND), PGA=001 (±4.096 V), MODE=1 (single shot),
        # DR=100 (128 samples/s), comparator off (COMP_QUE=11)
        config = 0x8000 | ((0x4 + channel) << 12) | (0x1 << 9) | (0x1 << 8) | (0x4 << 5) | 0x3
        self.bus.write_i2c_block_data(self.address, 0x01, [config >> 8, config & 0xFF])
        time.sleep(0.01)  # one conversion takes 7.8 ms at 128 samples/s
        hi, lo = self.bus.read_i2c_block_data(self.address, 0x00, 2)
        raw = (hi << 8) | lo
        if raw & 0x8000:
            raw -= 1 << 16
        return raw * 4.096 / 32768


def read_ds18b20():
    """Water temperature from the first DS18B20 on the 1-Wire bus (dtoverlay=w1-gpio), or None."""
    for path in glob.glob("/sys/bus/w1/devices/28-*/w1_slave"):
        try:
            lines = Path(path).read_text().splitlines()
        except OSError:
            continue
        if len(lines) == 2 and lines[0].strip().endswith("YES") and "t=" in lines[1]:
            return int(lines[1].split("t=")[1]) / 1000
    return None


class Rc522:
    """The RC522 reader on SPI0 with the mfrc522 library. Reads 4-byte UIDs (cards) and 7-byte UIDs (NTAG213
    filter tags: the library stops at the first cascade level, so the second one is done here)."""

    def __init__(self, rst_pin=25):
        from mfrc522 import MFRC522
        import RPi.GPIO as GPIO  # on a Pi 5: pip install rpi-lgpio, which provides it

        self.r = MFRC522(pin_mode=GPIO.BCM, pin_rst=rst_pin)

    def read_uid(self):
        r = self.r
        status, _ = r.MFRC522_Request(r.PICC_REQIDL)
        if status != r.MI_OK:
            return None
        status, cl1 = r.MFRC522_Anticoll()
        if status != r.MI_OK or len(cl1) != 5:
            return None
        if cl1[0] != 0x88:  # no cascade tag: a 4-byte UID
            return bytes(cl1[:4]).hex().upper()
        r.MFRC522_SelectTag(cl1)                                          # select level 1...
        r.Write_MFRC522(r.BitFramingReg, 0x00)
        status, cl2, _ = r.MFRC522_ToCard(r.PCD_TRANSCEIVE, [0x95, 0x20])  # ...then anticollision, level 2
        if status != r.MI_OK or len(cl2) != 5 or (cl2[0] ^ cl2[1] ^ cl2[2] ^ cl2[3]) != cl2[4]:
            return None
        return bytes(cl1[1:4] + cl2[:4]).hex().upper()


class PiHardware:
    """The real water point. NOT YET RUN ON THE BOARD: test every part with the probe command first."""

    TAP_GAP_S = 1.5   # a card held still is one tap: the same UID counts again only after this long away

    def __init__(self, cfg):
        from gpiozero import DigitalInputDevice, DigitalOutputDevice

        self.cfg = cfg
        self.valve = DigitalOutputDevice(cfg.valve_pin, active_high=cfg.valve_active_high, initial_value=False)
        self.flow_in = DigitalInputDevice(cfg.flow_pin, pull_up=True)
        self._pulses = 0
        self._lock = threading.Lock()
        self.flow_in.when_activated = self._pulse
        self.adc = Ads1115(cfg.i2c_bus, cfg.ads_address)
        self._taps = queue.Queue()
        self._running = True
        if cfg.reader == "rc522":
            self.reader = Rc522(cfg.rc522_rst_pin)
            threading.Thread(target=self._poll_reader, daemon=True, name="rc522").start()

    def _pulse(self):
        with self._lock:
            self._pulses += 1

    def _poll_reader(self):
        last_uid, last_seen = None, 0.0
        while self._running:
            try:
                uid = self.reader.read_uid()
            except Exception:  # a reader glitch must not stop the water point
                uid = None
            now = time.monotonic()
            if uid is not None:
                if uid != last_uid or now - last_seen > self.TAP_GAP_S:
                    self._taps.put(uid)
                last_uid, last_seen = uid, now
            time.sleep(0.1)

    def tap(self, uid):  # taps typed in the terminal (reader "none", or to test)
        self._taps.put(uid)

    def probe_volts(self):
        return self.adc.volts(self.cfg.tds_channel), self.adc.volts(self.cfg.ph_channel)

    def read_quality(self, now_ms):
        temp = read_ds18b20()
        try:
            v_tds, v_ph = self.probe_volts()
        except OSError:  # the ADC did not answer on I2C
            return Reading(now_ms, None, None, temp)
        tds = tds_from_volts(v_tds, 25.0 if temp is None else temp, self.cfg.tds_k)
        ph = ph_from_volts(v_ph, self.cfg.ph7_v, self.cfg.ph4_v)
        return Reading(now_ms, tds if v_tds > 0.005 else None, ph, temp)  # 0 V: the TDS probe is unplugged

    def take_flow(self):
        with self._lock:
            pulses, self._pulses = self._pulses, 0
        return pulses / self.cfg.pulses_per_l

    def take_taps(self):
        out = []
        while True:
            try:
                out.append(self._taps.get_nowait())
            except queue.Empty:
                return out

    def set_valve(self, open_):
        self.valve.value = bool(open_)

    def close(self):
        self._running = False
        self.valve.off()
        self.valve.close()
        self.flow_in.close()