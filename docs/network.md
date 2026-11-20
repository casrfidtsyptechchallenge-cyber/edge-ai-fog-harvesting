# Network intelligence (M8): the fog-front tracker

Fog reaches the nodes one after the other. From **when** each node detects it and **where** the nodes stand,
the station estimates the direction and speed of the fog front, shows it on the dashboard, and **pre-arms**
the nodes the fog has not reached yet. A pre-armed node lowers its detection threshold (0.30 → 0.20) until
`eta_s` + 60 s, so it starts collecting as soon as the fog arrives.

```
nodes: FOG_DETECTED, positions (info), clock (status), state (telemetry)
   │
   ▼
station/network ──► aeroion/station/front (retained) ──► dashboard: arrow, "fog in 12 s"
               └──► PREARM to each ARMED node ahead of the fog ──► CMD_ACK back
```

It works with the node-to-node FOG_ALERT (M7 part 3): that one pre-arms the neighbours even when the station
is down, with a fixed 60 s; the station's PREARM carries the expected arrival.

## How it estimates the front

A straight front at constant speed reaches the point (x, y) at `t = t0 + wx·x + wy·y`, where
`w = (sin d, cos d) / speed` and d is the direction it moves toward (0 = north, 90 = east, as in the
contract; x east, y north as in `info.position`).

| Nodes that saw it | What the tracker knows | Pre-arming the others |
|---|---|---|
| 1 | nothing yet, or the site's usual front | by distance at a slow speed (0.5 m/s); or the usual front's timing |
| 2, or more on one line | the speed along the line; across it, the usual front | the same |
| 3+ not on one line | direction and speed, by least squares | the fitted timing |
| 4+ | also how well a straight front fits (`fit_quality`, R² of the times) | the fitted timing |

Without the usual front, two nodes cannot tell a front moving along their line from a faster one crossing
it, so the tracker does not guess: it pre-arms by distance, with a long window rather than a short one.

**The site's usual front** (the prior) is set by hand (`--prior 90 1.0`: the fog usually moves toward the
east at 1 m/s, e.g. sea fog) or learned from the fronts measured on 3+ nodes, kept in
`data/network_prior.json`. It is used only while the fronts agree on a direction (agreement 0.7 or more,
and, when learned, after 3 fronts): at a site where fog comes from anywhere, it is never trusted.

**Pre-arming.** Only ARMED nodes (a LOCKED, SAFE or MAINTENANCE node refuses PREARM). A PREARM keeps a node
ready until `eta_s` + 60 s, so a second one is sent only when the expected arrival moves past that window.
The answers (CMD_ACK) are logged.

**Rules from the contract.** Nodes whose clock is not synced are not used (rule 3). A replayed detection
(`"buffered": true`) counts for the front it belongs to, but one older than 5 minutes does not start a new
front. A front ends once every node that saw it has cleared and the last expected arrival is 60 s past.
While a front is tracked, `aeroion/station/front` is published at each detection and every 10 s; at the end,
once more with `"active": false`.

## Run it

From the project folder, with Mosquitto running (four terminals, or three without the dashboard):

```bash
python sim/fake_node.py --nodes 3 --compress 4 --wind 90     # fronts moving toward the east, 4x faster
python -m station.network
python -m station.dashboard                                  # http://localhost:8080
```

The tracker's terminal shows each step:

```
13:12:13 fog at n01 (1 node(s) so far)
13:12:13 PREARM to n02: fog expected in 38 s (seen first at n01)
13:12:13 PREARM to n03: fog expected in 60 s (seen first at n01)
13:12:14 PREARM to n02: accepted
13:12:19 fog at n02 (2 node(s) so far)
13:12:21 fog at n03 (3 node(s) so far)
13:13:21 front over: 3 node(s), moving toward 58° at 3.18 m/s (fit)
```

Options: `--prior DEG MPS` (the site's usual front, by hand), `--forget` (start without the learned one),
`--no-prearm` (track and publish only), `--slow-speed` (default 0.5 m/s), `--broker`, `--port`.

For the demo with a fog machine at one end, give the direction it pushes the fog: with the usual front set,
the first node's detection is enough to show "fog in N s" at the others on the dashboard.

**On the Pi**, as a service next to the dashboard (adapt the folder and the user):

```ini
# /etc/systemd/system/aeroion-network.service
[Unit]
Description=AeroIon-Edge fog-front tracker
After=network-online.target mosquitto.service

[Service]
WorkingDirectory=/home/pi/aeroion-edge
ExecStart=/usr/bin/python3 -m station.network
Restart=on-failure
User=pi

[Install]
WantedBy=multi-user.target
```

If the tracker stops, the dashboard does not keep showing an old front: the tracker's MQTT last will (and
its first message at start) is "no front".

## How good is it

The simulator knows the true fronts (`--truth`). `score` replays a run through the tracker and compares:

```bash
python sim/fake_node.py --out run.jsonl --no-wait --duration 14400 --truth --seed 1
python -m station.network score run.jsonl
```

Four simulated hours each (open loop: the PREARMs are counted, not delivered):

| Run | Fronts | Direction error (median) | Speed error | Pre-armed in time | Warning | Arrival predicted within |
|---|---|---|---|---|---|---|
| 3 nodes, fog from any direction (`--seed 1`) | 34 | 2.0° | 3.5 % | 68 of 68 | 10.5 s | by distance (no usual front) |
| 5 nodes, any direction (`--nodes 5 --seed 2`) | 27 | 1.0° | 1.1 % | 107 of 107 | 22 s | 1.0 s (81 predictions) |
| 3 nodes, usual wind (`--wind 90 --seed 3`) | 25 | 2.9° | 1.9 % | 49 of 49 | 16 s | 2.6 s (65 predictions) |

"Pre-armed in time" counts the nodes reached after the first one that were ARMED: their PREARM was sent
before the fog came, and was still running when it came. Two nodes reached while SAFE or LOCKED are shown
apart: nothing can pre-arm them. With the wind, the usual front learned was 90° at 1.08 m/s, agreement 0.96.

Live (closed loop, 3 nodes, `--compress 4 --wind 90`): every PREARM accepted, the two fronts measured at 5°
and 5° from the truth, 2 % and 6 % on the speed. At 4× speed the fog crosses the three nodes in 6 to 8 s
while the nodes report once a second, so the error is larger than at real speed.

## Test

```bash
python -m pytest -q tests/test_network_front.py tests/test_network_engine.py
```

21 tests: the direction convention, 3 and 4 nodes, 2 nodes with and without the usual front, nodes on one
line, fog everywhere at once, pre-arming by distance and only ARMED nodes, PREARM sent again only when its
window runs out, the end of a front and the usual front learned, fronts from everywhere giving no usual
front, unusable detections (clock, position, too old), the messages in and out, duplicates and replays,
two simulator runs scored, the usual front kept in a file, the service.

## Files

| File | What |
|---|---|
| `station/network/front.py` | the fit: direction and speed from detection times, no numpy |
| `station/network/tracker.py` | one front at a time: detections, predictions, PREARM decisions, the usual front |
| `station/network/engine.py` | contract messages in and out |
| `station/network/service.py` | the live service on the broker; the usual front in a file |
| `station/network/score.py` | scoring against the simulator's truth |
| `station/network/__main__.py` | the command line |
| `sim/fake_node.py` | new option `--wind DEG`: fronts move toward DEG ±20° (a prevailing wind) |

## Limits to state openly

- A straight front at constant speed is a model. Real fog can curl around obstacles or thicken in place;
  `fit_quality` (4+ nodes) shows when the model does not fit. With 3 nodes nothing can be checked.
- Detection times carry the nodes' threshold and sampling (1 s): with nodes 15 m apart and a 1 m/s front
  that is a few degrees; it grows when the fog crosses the nodes in a few seconds.
- A pre-armed node detects a little earlier (lower threshold), which slightly biases the next fit.
- The energy planner (battery and fog forecast, F14, a stretch goal) is not part of this: the nodes run on
  bench power for now (`batt_pct` null).