# Water point (M9): quality gate, household cards, filter tag

The station's water point gives the collected water out, but only when it is safe. It runs on the Raspberry
Pi next to the broker and the dashboard.

```
probes (TDS, pH, temperature) ──► quality gate ──► verdict ok / bad / unknown
RFID reader (cards, filter tag) ──► water point ──► valve (normally closed)  ◄── flow meter (litres)
                                         │
                                         ├──► ledger (data/water.db, SQLite): every dispense and event
                                         └──► aeroion/station/water (retained) and water_event ──► dashboard
```

**The valve opens only while all of this is true:** the quality verdict is ok, a filter is fitted and not
used up, and a household card with quota left today has started a session. It closes as soon as one of them
stops being true.

## Quality gate (F7)

| Reading | Safe | Source |
|---|---|---|
| TDS (total dissolved solids, at 25 °C) | up to 500 ppm | the usual drinking-water guide value (WHO: under about 600 mg/L tastes good) |
| pH | 6.5 to 8.5 | WHO and most national standards |

- The probes are read once a second. The gate uses the median of the last 3 readings, so one wrong reading
  changes nothing.
- **bad** as soon as that median is out of limits: two bad readings in a row, about 2 s.
- **ok** only after 3 s of good readings in a row (a probe just put back in the water reads wrong at first).
- **unknown** when a probe gives nothing for 5 s, or a value no probe can give (pH above 14): the valve stays
  closed, as for bad.

Fog water is nearly free of minerals: TDS is low (tens of ppm) and the pH often slightly acidic. A pH below
6.5 is the likely failure here, from the collector's materials or dirt.

## Cards and tags (F13)

| Tapped | What happens |
|---|---|
| household card (badge list, role `household`) | a session starts if the water is safe, a filter is fitted and the card has quota left; tapped again, it ends |
| another household card during a session | refused: "busy" |
| technician or admin badge | for 30 s, the water point waits for a filter's tag |
| registered filter tag, after that badge | fitted (FILTER_CHANGE); refused if used up |
| filter tag without the badge | refused |
| operator badge, unknown card | refused |

**The household quota** is 20 L a day per card (`--quota` changes the default; `quota` gives one card its
own). It starts again at the station's local midnight and is counted from the ledger, so a restart does not
give it again.

**A session ends**, and the valve closes, when the card's quota is reached, the card is tapped again, no
water flowed for 15 s (the person left, or the tank is empty), it lasted 10 minutes, the quality verdict
stops being ok, the filter reaches its capacity, or the water point is stopped. The DISPENSE event says
which.

**The filter cartridge** carries an NTAG213 sticker. The admin registers each tag with its capacity
(`filter add`): only a registered tag is accepted (the "genuine" check). The water point counts the litres
through it, warns at 90 % ("expiring"), and at 100 % closes the valve and refuses every card until a
technician fits a new one (FILTER_EXPIRED).

**Privacy.** A card is shown by its label (`card 7`), or by the end of its UID (`…B2C3`): the dashboard and
the messages never carry a household card's whole UID. Labels must stay anonymous (no names).

## Messages (contract 1.2)

| Topic | When | What |
|---|---|---|
| `aeroion/station/water` (retained) | at once on a change, every second while water flows, every 5 s otherwise | quality and verdict, valve, session (card, litres, quota left), filter, water given out today |
| `aeroion/station/water_event` | when it happens | `DISPENSE`, `CARD_DENIED`, `QUALITY_CHANGE`, `FILTER_CHANGE`, `FILTER_EXPIRED` |

Field by field: [contract-reference.md](contract-reference.md). Every event is written to the ledger first.
If the broker is unreachable, the water point keeps working, and the events it could not publish are sent
when the broker is back, with `"buffered": true` (logged offline, synced later).

## Try it on the PC

From the project folder, with Mosquitto running:

```bash
python -m station.water --sim
python -m station.dashboard            # another terminal: http://localhost:8080, the Water point card
```

The first `--sim` run sets up demo cards and filters in `data/sim/` (the real `data/badges.json` and
`data/water.db` are never touched): household `card 7` (20 L a day) and `card 8` (2 L, to see a quota run
out), technician `blue key fob`, operator `white card`, `filter 3` fitted (112 L used), `filter 4` new and
`filter 2` used up. Water flows at 10 L/min (`--flow`). Then type:

```
tap card 7          valve opens: "card 7: valve open, 20.00 L left today"
tap card 7          valve closes: "card 7 drew 1.52 L, 18.48 L left today (valve closed: card)"
tap card 8          after 12 s: "card 8 drew 2.01 L, 0.00 L left today (valve closed: quota)"
tap card 8          "card 8: no water (daily quota used (2 L))"
tds 940             "water quality BAD: TDS 940 ppm, above 500"; a running session closes
tap card 7          "card 7: no water (water not safe: TDS 941 ppm, above 500)"
tds 90              3 s later: "water quality OK"
tap filter 4        "filter 4: no water (filter tag: a technician's badge first)"
tap blue key fob    then: tap filter 2 (refused: used up), tap filter 4 (fitted)
probes out          5 s later: "water quality UNKNOWN: no reading from the probes"
probes in
flow 0              the tank is empty: a session closes after 15 s with "idle"
status, cards, help, quit
```

Demo step 6 ("water point: quality verdict, then a household card draws its quota"): show the card at
"Safe to drink", tap a household card, watch the litres climb on the dashboard; `tds 940` turns it to
"NOT SAFE" and closes the valve.

## Admin

```bash
python -m station.badges add 0A1B2C3D household --label "card 7"     # a household card (badge list, M4)
python -m station.water quota "card 7" 25                            # its own quota: 25 L a day
python -m station.water quota "card 7" --default
python -m station.water quotas
python -m station.water filter add 04C3A1B2E35F80 --capacity 500 --label "filter 3"
python -m station.water filter list
python -m station.water report                                       # today: per card, refusals, quality
python -m station.water report --day 2026-11-20
```

These work while the water point runs (same SQLite file). Add `--db data/sim/water.db --badges
data/sim/badges.json` to look at the simulator's ledger.

## On the Raspberry Pi

**Parts** (one water point):

| Part | Connects to | Note |
|---|---|---|
| 12 V solenoid valve, **normally closed** | relay or logic-level MOSFET on GPIO17 (pin 11) | flyback diode across the coil |
| YF-S201 flow meter | GPIO27 (pin 13), through 10 kΩ / 20 kΩ | its output swings to 5 V: the divider brings it to 3.3 V |
| ADS1115 ADC | I2C (pins 3 and 5), 3.3 V | address 0x48 (ADDR to GND) |
| Gravity analog TDS meter | ADS1115 A0 | |
| Gravity analog pH meter V2 | ADS1115 A1 | |
| DS18B20 water temperature | GPIO4 (pin 7), 4.7 kΩ pull-up | for the TDS temperature correction |
| RC522 reader | SPI0 (pins 19, 21, 23, 24), RST on GPIO25 (pin 22), **3.3 V** | the same module as the nodes' |

**The valve must stay closed whenever the program is not driving it**: while the Pi boots, after a crash,
with the power off. Use a normally-closed valve and a driver that is off when its input is low (an
active-high relay module or a MOSFET). Many cheap relay modules are active-low: on those the valve would
open while the Pi boots. `valve_active_high` stays `true`.

**Set up** (Raspberry Pi OS):

```bash
sudo raspi-config nonint do_i2c 0 && sudo raspi-config nonint do_spi 0
echo "dtoverlay=w1-gpio" | sudo tee -a /boot/firmware/config.txt    # DS18B20, then reboot
python -m pip install gpiozero smbus2 spidev mfrc522
python -m pip install rpi-lgpio        # Pi 5 only: replaces RPi.GPIO, which the RC522 library needs
```

Pins and calibration live in `data/water_point.json` (every key optional; the defaults are in
`station/water/hardware.py`, `PiConfig`):

```json
{"valve_pin": 17, "flow_pin": 27, "pulses_per_l": 450, "ph7_v": 1.500, "ph4_v": 2.032, "tds_k": 1.0}
```

**Check every part on the bench first** (the valve stays closed):

```bash
python -m station.water probe
```

It prints the probe voltages and values, the flow meter's pulses and the taps every second.
- pH: probe in the pH 7.00 buffer, copy the voltage to `ph7_v`; in the pH 4.00 buffer, to `ph4_v`.
- TDS: in a known solution (e.g. 342 ppm), set `tds_k` = known / shown.
- Flow: run exactly 1 L into a jug, set `pulses_per_l` to the pulses counted.
- Reader: tap a card and a filter tag; both UIDs must appear (4 and 7 bytes).

Then run it, and as a service next to the others (adapt the folder and the user):

```ini
# /etc/systemd/system/aeroion-water.service
[Unit]
Description=AeroIon-Edge water point
After=network-online.target mosquitto.service

[Service]
WorkingDirectory=/home/pi/aeroion-edge
ExecStart=/usr/bin/python3 -m station.water
Restart=on-failure
User=pi

[Install]
WantedBy=multi-user.target
```

`systemctl stop` closes the session, records it, closes the valve and publishes "water point stopped".

## Test

```bash
python -m pytest -q tests/test_water_quality.py tests/test_water_point.py
```

32 tests: the gate (settling, two bad readings, one wrong reading ignored, both pH limits, silent and broken
probes, the probe formulas); sessions and every reason the valve closes; quotas per card and per day; refused
cards; the filter change, a filter that runs out mid-session; the ledger across a restart; card names never
showing the whole UID; the badge list read again; the service (valve, messages every second while flowing,
events kept while the broker is down and sent later, an error closing the valve, a clean stop); the admin
commands; the simulator set-up. Every message is checked against the contract. Deliberately breaking the
quality rule, the closing on bad water or the closing on an error makes a test fail.

## Files

| File | What |
|---|---|
| `station/water/quality.py` | the quality gate: readings in, verdict out |
| `station/water/point.py` | cards, sessions, quotas, filter changes; the badge list read again when it changes |
| `station/water/ledger.py` | the SQLite record |
| `station/water/hardware.py` | the simulated water point; the Pi parts (valve, flow meter, ADS1115, DS18B20, RC522) |
| `station/water/service.py` | the loop (10 times a second) and the messages |
| `station/water/__main__.py` | the command line: run, probe, quota, filter, report |

## Limits to state openly

- **The Pi parts have not been run on the board yet.** They follow the datasheets and the libraries' code;
  `probe` is there to check them one by one.
- TDS and pH are not a full water analysis: no bacteria, no heavy metals. They catch a contaminated or acidic
  tank, not everything. A real site would add UV disinfection and lab tests.
- A filter tag is checked by its UID, which can be cloned. NTAG213 also has a factory signature (READ_SIG)
  that would prove the chip is genuine; it is not read yet.
- The litres of a session are written when it ends: if the Pi loses power during a session, that session's
  litres are not counted against the quota or the filter.
- If the program hangs without crashing, the valve keeps its state. The idle and session limits only work
  while the loop runs; a hardware watchdog would cover a hang.