# AeroIon-Edge

A network of electrostatic fog collectors with the intelligence at the edge. In each collector, a high
voltage (HV) charges the fog droplets and a grounded mesh catches them. Each node decides on its own when
the HV may be on, how high to set it and when an arc is coming. A Raspberry Pi station tracks the fog
front, records the data, runs the dashboard and gives the water out at a water point. No internet is
needed anywhere.

> Competition rule: keep every file anonymous. No names, university, branch, logos or emails in code,
> comments, commit messages or documents.

## Status

| Module | What | Runs on | State |
|---|---|---|---|
| **M0** | Message contract and node simulator | everywhere | done |
| **M1** | Sensing: collector current, optical fog, radar, lid, temperature and humidity, load cell, power | MCX (fast), ESP32-S3 (slow) | done |
| **M2** | Data logging, labels and datasets | station or laptop | done |
| **M3** | Safety core: the state machine that decides the HV | MCX | done |
| **M4** | RFID badges: arming, maintenance lock-out | MCX + station | done |
| **M5** | Edge AI: 5a arc predictor (8-bit CNN) with its novelty guard ("knows when it does not know"), 5b virtual fog sensor, 5c electrode health | MCX (5a), ESP32-S3 (5b, 5c) | done |
| **M6** | HV controller: the most water per watt-hour | ESP32-S3 | done |
| **M7** | Links: UART between the two chips, MQTT to the station, ESP-NOW fog alerts between nodes | MCX + ESP32-S3 | done |
| **M8** | Fog-front tracker: direction, speed, pre-arming the nodes ahead of the fog | station | done |
| **M9** | Water point: quality gate, household cards, filter tags | station | done |
| **M10** | Dashboard, with a jury view | station | done |
| **M11** | Training pipeline: recorded runs to a retrained arc predictor in the firmware | laptop | done |

**Waiting for the hardware:** the HV stage (HV output on the MCX, current-sense calibration) and the first
runs in the fog chamber. Until then the AI models are trained on synthetic data (see [Limits](#limits)).

## How it works

```
NODE (one per collector)
  MCX A156: the safety controller           ESP32-S3: the radio and the slower models
    M1  fast sensors: collector current       M7  Wi-Fi and MQTT to the station
        at 10 kHz, optical fog, radar, lid    M7  ESP-NOW fog alerts to the neighbours
    M3  safety core: decides the HV           M5b virtual fog sensor, M5c electrode health
    M5a arc predictor, 8-bit CNN              M6  HV controller (it only requests)
    M4  badges (RC522 reader)                 M1  slow sensors: SHT45, load cell
                 |                                          |
                 +------------- UART, JSON lines -----------+
                                                            |
                                                            | MQTT over Wi-Fi
STATION (Raspberry Pi, its own hotspot, no internet)        |
  Mosquitto broker <----------------------------------------+
  M2 data log, M4 badge list, M8 fog-front tracker, M9 water point, M10 dashboard

LAPTOP
  M11 training pipeline ---> arc_model_data.h ---> MCX firmware
```

Three rules hold everywhere:

- **The safety core decides, everything else asks.** The AI models, the controller, the station and the
  badges only make requests. Only the safety core on the MCX switches the HV, and it cuts it at once for a
  person, an open lid, an arc or a fault.
- **Each node works alone.** Arc prediction, safety, the HV level and the badge list live on the node. When
  the station or the radio is down, the node keeps working. When the ESP32 restarts, the MCX keeps its
  safety rules and goes back to its default level.
- **One contract.** Every module talks through the messages defined in `contract/`: JSON over MQTT, and one
  16-byte binary alert between nodes. The simulator, the tests and both firmwares follow it.

## Repository

```
contract/                M0: the message contract
  schemas/                 one JSON Schema per message type: the source of truth
  binary/fog_alert.json    the 16-byte FOG_ALERT sent between nodes
  examples/                one valid example of every message
  aeroion_contract.py      Python: validate(), topics, enums, FOG_ALERT pack/unpack
  generate.py              regenerates the C header and docs/contract-reference.md
firmware/
  mcx_node/                MCX A156 firmware: sensing, safety core, badges, arc predictor, UART link
  mcx_bench/               FRDM-MCXA156 board files and the bench firmware used to bring up M3
  node/                    PlatformIO project: the ESP32-S3 firmware and the C++ shared by both chips
    lib/                     portable C++: safety, sensing, drivers, rfid, edge_ai, controller,
                             gateway, link, telemetry
    src/link/                the ESP32-S3 of the two-chip node
    src/                     single-chip sensor firmware (M1 and M3 on one ESP32, for bench tests)
    test/                    PC tests of every library
    include/                 generated contract header, node settings, wifi_secrets.example.h
    wokwi/, diagram*.json    Wokwi simulation of the ESP32
sim/fake_node.py         simulated nodes: valid messages, fog fronts, arcs, commands obeyed
station/                 the Raspberry Pi services (Python)
  datalog/                 M2: record runs, build the datasets
  badges/                  M4: the badge list sent to the nodes
  network/                 M8: fog-front tracker
  water/                   M9: water point
  dashboard/               M10: web dashboard
ml/                      laptop: arc physics, CNN training and evaluation (M5a), pipeline (M11)
  models/                  every trained model; the one in the firmware is named in arc_model_data.h
tools/                   contract checker, command sender, serial bridge and capture, MCX simulator
tests/                   Python tests
docs/                    one document per module (list below)
```

Not in git: `data/` (recorded runs, water ledger), `firmware/node/include/wifi_secrets.h` (Wi-Fi
password), build folders.

## Try it without hardware

### Install (once)

Python 3.10 or newer. On Windows type `python` (or `py`), on macOS and Linux `python3`. Run everything from
the project folder.

```bash
python -m pip install -r requirements-ml.txt    # the station's packages, plus numpy for the models
python -m pytest -q                             # 281 tests (2-3 skip without a C compiler or scikit-learn)
```

On the Raspberry Pi, `requirements.txt` is enough (the model tests are then skipped).

### The whole site on one computer

Install Mosquitto: Raspberry Pi or Ubuntu `sudo apt install mosquitto mosquitto-clients`, macOS
`brew install mosquitto`, Windows the installer from mosquitto.org (it runs as a service). Then one terminal
for each line:

```bash
mosquitto -v                                               # the broker (skip it if it runs as a service)
python sim/fake_node.py --nodes 3 --compress 4 --wind 90   # three nodes, fog fronts moving east, 4x faster
python -m station.network                                  # M8: tracks the fronts, pre-arms the nodes ahead
python -m station.water --sim                              # M9: the water point with demo cards and filters
python -m station.dashboard                                # M10: http://localhost:8080 (jury view: ?view=jury)
```

Send commands as the station does:

```bash
python tools/send_cmd.py --node n02 lock "testing the stop button"
python tools/send_cmd.py --node all hint 0.6
python tools/send_cmd.py --node n01 tare
```

What to watch: each node goes `LOCKED`, then `ARMED` (a simulated badge), then `HARVEST` when the fog
arrives; the dashboard shows the front's direction and "fog in N s" at the nodes ahead; `ARC_PREDICTED`
events are followed by `BACKOFF`; one node's electrode health slowly drops.

### Without a broker

```bash
python sim/fake_node.py --out run.jsonl --no-wait --duration 600 --seed 1   # 10 simulated minutes
python tools/contract_check.py --no-clock run.jsonl                         # every message against the contract
python -m station.dashboard --replay run.jsonl --speed 8                    # replay it in the dashboard
```

In PowerShell use `--out` as above, not `>`: Windows PowerShell saves redirected output as UTF-16.

## Firmware

| What | How | Details |
|---|---|---|
| Tests of the node libraries, on the PC | `cd firmware/node`, then `pio test -e native` (208 tests) | |
| ESP32-S3 of a node | copy `include/wifi_secrets.example.h` to `include/wifi_secrets.h` and fill it in, then `pio run -e esp32s3-link -t upload` | [link.md](docs/link.md) |
| ESP32-S3 in Wokwi, no board | env `esp32s3-link-wokwi` and `python tools/mcx_sim.py` | [link.md](docs/link.md) |
| MCX A156 | `west build -b frdmmcxa156 <repo>/firmware/mcx_node`, then `west flash` (MCUXpresso SDK) | [mcx_node.md](docs/mcx_node.md) |
| MCX without an ESP32 | `python tools/serial_bridge.py --port COM5`: the PC does the ESP32's job | [link.md](docs/link.md) |

## Data and models (laptop)

```bash
# record a run from the broker (Ctrl-C to stop), then build its tables in data/runs/<run>/dataset
python -m station.datalog record --tag gap15 --set gap_mm=15 --set controller=m6
python -m station.datalog build latest

# M11: retrain on every recorded run, compare, update the firmware only if the new model is better
python -m ml pipeline
python -m ml models                 # every model trained, and the one in the firmware
```

To try the pipeline before the chamber exists: `python -m ml demo-runs`, then
`python -m ml pipeline --data data/demo-runs --dry-run`.

```bash
python -m ml novelty                # the arc predictor's novelty guard: how it reacts to unseen conditions
python -m ml fog-forecast data/metar/*.csv   # fog within 1 h / 3 h, from real METAR reports (Tunisia ASOS)
python -m ml adapt                  # experiment: on-node learning from the node's own arcs (not in the firmware)
```

For training-data campaigns in the chamber, put the MCX in `ai observe` (the predictor computes and reports,
the safety core does not act on it; arc detection stays on) and record periodic windows (`waves 2`): windows
followed by a back-off on a prediction are left out of training (intervention bias, [training.md](docs/training.md)).

Every push runs the CI (`.github/workflows/ci.yml`): the Python tests, the node libraries' PC tests, and a
Cortex-M33 compile of the portable C++.

## Documents

| Document | Module |
|---|---|
| [message-schema.md](docs/message-schema.md) | M0: topics, units, null, time, versions, how to change the contract |
| [contract-reference.md](docs/contract-reference.md) | M0: every field of every message (generated) |
| [mcx_node.md](docs/mcx_node.md) | M1, M3, M4, M5a, M7 on the MCX: wiring, build, bench test |
| [datasets.md](docs/datasets.md) | M2: run folders and tables |
| [safety_core.md](docs/safety_core.md) | M3: states, transitions, limits |
| [rfid.md](docs/rfid.md) | M4: badges and roles |
| [edge_ai.md](docs/edge_ai.md) | M5: arc predictor, virtual fog sensor, electrode health |
| [controller.md](docs/controller.md) | M6: the HV controller |
| [link.md](docs/link.md) | M7: UART, MQTT, ESP-NOW fog alerts, Wokwi |
| [network.md](docs/network.md) | M8: fog-front tracker |
| [water_point.md](docs/water_point.md) | M9: water point |
| [dashboard.md](docs/dashboard.md) | M10: dashboard and jury view |
| [training.md](docs/training.md) | M11: training pipeline |

## Limits

- **The AI models are trained on synthetic data** (physics-based simulators) until the fog chamber records
  real runs. Their numbers show that the chain works end to end, not how well it does in the field. M11
  retrains them on the recorded runs.
- **The HV stage is not built yet.** On the MCX, `hv_level` is the level the safety core allows; 5b and 5c
  need the real current to mean anything. Once it is wired, set `AEROION_HAS_HV_OUTPUT 1` in
  `mcx_node_config.h`: that turns on the check "no current with the HV on = disconnected sensor".
- **The links are bench-grade:** MQTT without passwords or TLS, and FOG_ALERT is not signed. Field version:
  TLS and a password per node, signed alerts, and LoRa for long distances (the 16-byte FOG_ALERT already
  fits in a LoRa packet).

## Changing the contract

Read [docs/message-schema.md](docs/message-schema.md#how-to-change-the-contract). In short: agree first,
edit the schema, raise the version, run `python contract/generate.py`, run the tests, commit everything
together.
