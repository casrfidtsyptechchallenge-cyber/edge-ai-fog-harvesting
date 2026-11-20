# Station dashboard (M10)

A web page served by the Raspberry Pi on its own hotspot, so it needs no internet. Phones, laptops and the
station screen open it in a browser. It shows the live state of every node and of the whole site, and
there is a jury view for the demo.

```
nodes ── MQTT broker (Pi) ── station/dashboard (Python) ── HTTP + live stream ── browsers on the hotspot
                                     │
                     recorded run ───┘ (--replay: no broker needed)
```

## What it shows

| Part | From | What |
|---|---|---|
| Water collected (hero) | `telemetry.mass_g`, `rate_gpm` | water on all collectors since their last TARE, and the rate now |
| Nodes harvesting | `telemetry.state`, the station's clock | nodes in HARVEST, and nodes sending data |
| Water per energy | `mass_g` / `e_wh` | grams per watt-hour (water since TARE, energy since boot: an estimate) |
| Arc warnings from the AI | `ARC_PREDICTED`, `ARC` events | warnings that made a node back off; arcs that still happened, and how many came without a warning |
| Fog front | `aeroion/station/front` (M8) | direction, speed, fit, and the next node the fog will reach |
| Site map | `info.position`, `front` | nodes where they stand (north up), fog around each (grey halo), the front's arrow, "fog in 12 s" |
| Needs attention | everything | offline or silent nodes, SAFE, maintenance, person near, lid open, electrode health, clock not synced, battery, offline buffer; water not safe or unknown, filter nearly or fully used up, water point silent |
| Water point | `aeroion/station/water` (M9) | the quality verdict and why, TDS, pH, water temperature, the valve, who is drawing water and their quota left, the filter's litres, water given out today. Shown once the water point runs |
| Charts (last 10 min) | `telemetry` | fog index, water and collector current, one line per node; hover or focus for the values |
| Node cards | `telemetry`, `status`, `info` | state and why, fog (optical and virtual), water, current, HV level, electrode health, arc risk, power, air, safety sensors, link, battery |
| Events | `event`, `cmd`, `water_event` | every event, every command sent by any station tool and every water-point event (key `water`), newest first |

Each node keeps one colour everywhere (n01 blue, n02 orange, n03 aqua...); its id is always written next to
it, so the colour is never the only way to tell nodes apart. States and alerts show a shape and a word
with their colour.

Rules from the contract that the dashboard follows:

- **Replayed messages** (`"buffered": true`) go into the charts and the events at their own time, marked
  "replayed", but never show as live values (rule 5).
- **A message received twice counts once** (same node, seq and time; for the water point, same type and time).
- **Silence is judged on the station's clock**, not the node's: a node whose clock is wrong still shows as
  live while it sends. No telemetry for 5 s: "silent". Last will: "offline".
- **Tolerant** (rule 8): unknown fields, event types and versions 1.x are accepted; a wrong-typed value
  shows as "–". Text from the nodes is never interpreted as HTML.

## Run it

From the project folder (`python -m pip install -r requirements.txt` once; nothing else is needed).

**On the PC, with the simulator** (three terminals):

```bash
mosquitto -v                                      # on Windows: & "C:\Program Files\mosquitto\mosquitto.exe" -v
python sim/fake_node.py --nodes 3 --compress 8
python -m station.dashboard
```

Open <http://localhost:8080>. Within a few seconds the three nodes appear; fog comes every half minute or so
with `--compress 8`. The fog front appears once the tracker (M8) runs, the water point once M9 runs
(`python -m station.water --sim`, docs/water_point.md).

**A recorded run, without a broker:**

```bash
python sim/fake_node.py --out run.jsonl --no-wait --duration 900 --seed 1
python -m station.dashboard --replay run.jsonl --speed 8
```

`--replay` also reads the `raw.jsonl` of a station.datalog run. `--speed 0` loads everything at once. This
is the backup for the demo: the same page, from a recording.

**On the Pi:**

```bash
export AEROION_DASHBOARD_PASSWORD='choose-one'    # PowerShell: $env:AEROION_DASHBOARD_PASSWORD = "choose-one"
python -m station.dashboard --host 0.0.0.0
```

Phones on the hotspot open `http://<the Pi's address>:8080`. With the password set, the browser asks for it
(any user name). Without it, anyone on the hotspot can see the page and use LOCK and TARE; the command line
says so.

To start it with the Pi, a systemd service (adapt the folder and the user):

```ini
# /etc/systemd/system/aeroion-dashboard.service
[Unit]
Description=AeroIon-Edge dashboard
After=network-online.target mosquitto.service

[Service]
WorkingDirectory=/home/pi/aeroion-edge
Environment=AEROION_DASHBOARD_PASSWORD=choose-one
ExecStart=/usr/bin/python3 -m station.dashboard --host 0.0.0.0
Restart=on-failure
User=pi

[Install]
WantedBy=multi-user.target
```

Then `sudo systemctl enable --now aeroion-dashboard`.

Options: `--broker`, `--port` (MQTT), `--http-port` (default 8080), `--history-min` (default 10),
`--read-only` (no command buttons). `python -m station.dashboard --help` lists them.

## Jury view

The **Jury view** button, or `?view=jury` in the address: the totals, the map, fog and water in large type,
readable from across the room; the event list and the details are hidden. **Theme** switches light, dark
or the system's setting (remembered by each browser).

## Commands from the page

Only two, and neither can switch the HV on:

| Button | Command | Effect |
|---|---|---|
| **LOCK all nodes** (top) / **LOCK** (node card) | `LOCK` | the node goes LOCKED now: HV off, an operator badge is needed to arm again |
| **TARE** (node card) | `TARE` | the node's load cell restarts from 0 g |

Each asks for confirmation. The node's answer (CMD_ACK) appears in the events: `accepted`, or `rejected`
with the reason. PREARM stays with the fog-front tracker (M8), and the HV level with each node's controller
(M6, `docs/controller.md`). Commands need the live broker: in a replay the buttons are hidden.

The server only accepts commands that carry the page's own header, so a link or a form on another site
cannot send one through a visitor's browser.

## Test

```bash
python -m pytest -q tests/test_dashboard_state.py tests/test_dashboard_server.py
```

25 tests: every contract example, live versus replayed values, duplicates, alerts and their order, silence
on the station's clock, the chart window, broken and foreign input, the front, the water point (status,
alerts, events, an old retained status), the page and its files, the
live stream, the commands (only LOCK and TARE, the header, no broker, the body read before any answer), the password, a recorded run played
back, a slow browser dropped, the broker status.

By hand, with the simulator running:

1. The three nodes appear; `n01` blue, `n02` orange, `n03` aqua on the map, the charts and the cards.
2. Fog arrives: the fog chart rises node after node, nodes go HARVEST, water climbs.
3. A node's radar sees someone (the simulator does it now and then): SAFE in red in "Needs attention".
4. **LOCK** on a node card, confirm: the events show `command LOCK dash-… sent`, then the node's
   `→ LOCKED (station LOCK: …)` and `command dash-… accepted`.
5. Stop the simulator with Ctrl-C: every node goes "offline" within a second.
6. Stop the broker: the top bar turns red ("broker … lost, reconnecting"); start it again: green.

## Files

| File | What |
|---|---|
| `station/dashboard/state.py` | what is shown, from contract messages: no network, tested on its own |
| `station/dashboard/server.py` | MQTT and replay sources, the page, the live stream (Server-Sent Events), commands, password |
| `station/dashboard/__main__.py` | the command line |
| `station/dashboard/static/` | the page: `index.html`, `style.css`, `app.js` (no library, nothing from the internet) |
| `tests/test_dashboard_state.py`, `tests/test_dashboard_server.py` | the tests |

## Limits to state openly

- The page is plain HTTP. The password (HTTP Basic) is protected only by the hotspot's own Wi-Fi
  encryption (WPA2). A real site would add HTTPS.
- The history is in memory: 10 minutes for the charts, 200 events. Restarting the dashboard starts them
  again (the retained `info`, `status` and `front` come back at once). Long-term records are M2's job.
- "Water per energy" divides water since the last TARE by energy since the node's boot: an estimate.
- The fog front appears only when the tracker (M8) publishes it, the water point only when M9 runs.