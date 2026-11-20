# AeroIon-Edge message contract (M0)

Every module talks to the others only through the messages defined here. If two modules agree with this
contract, they work together, whoever wrote them and whenever.

- **Source of truth:** the JSON Schemas in `contract/schemas/`.
- **Field-by-field reference:** [contract-reference.md](contract-reference.md), generated from the schemas.
- **Firmware header:** `firmware/node/include/aeroion_contract.h`, generated from the schemas.
- **Examples:** one valid message per type in `contract/examples/`.

## Topic map

| Topic | From → to | When | Retained |
|---|---|---|---|
| `aeroion/{node_id}/telemetry` | node → station | every second | no |
| `aeroion/{node_id}/event` | node → station | when something happens | no |
| `aeroion/{node_id}/status` | node → station | every 10 s, and the last will | yes |
| `aeroion/{node_id}/info` | node → station | once at boot | yes |
| `aeroion/{node_id}/wave` | node → station | around arcs (debug, training) | no |
| `aeroion/{node_id}/cmd` | station → one node | on demand | no |
| `aeroion/all/cmd` | station → every node | on demand | no |
| `aeroion/station/front` | station → dashboard | while a fog front is tracked | yes |
| `aeroion/station/water` | station → dashboard | water point status: every 5 s (1 s while water flows), at once on a change | yes |
| `aeroion/station/water_event` | station → dashboard | a dispense, a refused card, a quality or filter change | no |
| `FOG_ALERT` (binary, ESP-NOW/LoRa) | node → nodes | on fog detection | — |

`aeroion/sim/...` is used only by the simulator (ground truth for testing) and is not part of the contract.

Node ids are `n01` to `n99`. The station is the Raspberry Pi; it runs the MQTT broker (Mosquitto) on its
own Wi-Fi hotspot, so no internet is needed.

## Rules everyone follows

1. **Units are in the field name.** `_c` °C, `_pct` %, `_g` grams, `_gpm` g/min, `_ua` µA, `_w` W,
   `_wh` Wh, `_s` seconds, `_ms` milliseconds, `_m` metres, `_deg` degrees, `_mps` m/s, `_dbm` dBm.
   Fractions such as `fog_idx_opt`, `hv_level` and `arc_risk` run from 0 to 1.
2. **Missing means `null`, never a fake 0.** A failed or absent sensor reports `null`. A 0 must be a real
   measurement.
3. **Time is Unix milliseconds, UTC** (`ts`). Nodes take their time from the station (NTP on the Pi, or
   sync beacons over ESP-NOW) and report `time_synced` in `status`. The fog-front tracker ignores nodes
   that are not synced.
4. **Every node counts its messages** (`seq`, +1 for each telemetry, event and wave message). The station
   detects lost messages from gaps and duplicates from repeats.
5. **Offline buffering.** When the link to the station is down, the node keeps working and stores its
   messages. When they are replayed later they carry `"buffered": true`, so the dashboard does not show
   old data as live.
6. **Commands are requests, not orders.** The station sends `cmd`; the node always answers with a
   `CMD_ACK` event (`accepted`, `rejected` with a reason, or `expired`). The safety core (M3) can refuse
   anything. A command is void after `ts + ttl_s`.
7. **Online / offline.** On connect, each node registers an MQTT *last will* on its `status` topic:
   `{"schema_version": "1.0", "node_id": "n01", "online": false}`. If the node crashes or loses power,
   the broker publishes it. On a clean shutdown the node publishes `online: false` itself.
8. **Be strict when sending, tolerant when receiving.** Senders must produce exactly what the schemas
   allow: the checker rejects unknown fields, which catches typos. Receivers (dashboard, station
   services) must ignore fields and enum values they do not know, so a newer node does not break an
   older station.

## Versioning

`schema_version` is `MAJOR.MINOR`, currently `1.3` (1.2 added the water point's two messages; 1.3 added the optional
telemetry field `arc_novel` and the wave trigger `NOVEL`, from the arc predictor's novelty guard). It lives in `contract/schemas/common.schema.json`
(`x-aeroion-version`).

| Change | Version bump | Example |
|---|---|---|
| Add an optional field, a new event or command type, a new enum value | MINOR (1.0 → 1.1) | add `wind_mps` to telemetry |
| Rename or remove a field, change a unit or meaning, make a field required | MAJOR (1.x → 2.0) | rename `mass_g` to `water_g` |

Consumers accept any message with the same MAJOR. Avoid MAJOR changes after G1 (1 November): every
track would have to update at once.

## How to change the contract

1. Agree on the change with the other two tracks first.
2. Edit the schema in `contract/schemas/` (and `common.schema.json` if you add an enum value). Bump
   `x-aeroion-version` following the table above.
3. Update or add an example in `contract/examples/`.
4. Regenerate: `python contract/generate.py`.
5. Run the tests: `python -m pytest -q`.
6. Update the simulator if it should produce the new field.
7. Commit everything together, with the version in the message (for example *"contract 1.1: add
   wind_mps to telemetry"*).

## Node state machine (what the `state` field means)

| State | High voltage | Meaning |
|---|---|---|
| `LOCKED` | cannot turn on | nobody authorised the node (default after power-up) |
| `ARMED` | off, allowed | an operator badge authorised it; waiting for fog |
| `HARVEST` | on, AI-controlled | fog present; the controller tunes the level within M3's limits |
| `BACKOFF` | reduced | an arc was predicted or happened; ramp down, then back up |
| `SAFE` | forced off | person nearby, lid open, fault or watchdog |
| `MAINTENANCE` | forced off, locked | technician lock-out; only the same badge releases it |

Every change is published as a `STATE_CHANGE` event with a short reason.

## Fog-front tracking needs non-collinear nodes

The tracker (M8) estimates the direction and speed of a fog front from the times each node detects it.
With three nodes on one straight line, only the speed along that line can be measured. Place at least
three nodes as a triangle. The simulator does this by default (15 m apart, alternating y = 0 and 12 m).

## The water point (station messages)

The water point (M9) runs on the station, so its messages carry no `node_id` and no `seq`. A card is shown
by its label, or by the end of its UID (`…B2C3`): the whole UID of a household card never leaves the Pi.
`water_event` messages sent again after the broker was unreachable carry `"buffered": true`, like a node's
replayed messages (rule 5). Details: [water_point.md](water_point.md).

## FOG_ALERT (node to node, binary)

A 16-byte packed little-endian struct broadcast over ESP-NOW (demo) or LoRa (field) when a node detects
fog, so neighbours can prepare even if the station is down. The layout is in
[contract-reference.md](contract-reference.md#fog_alert-binary-node-to-node); the C struct is
`aeroion_fog_alert_t` and Python uses `pack_fog_alert()` / `unpack_fog_alert()`.
