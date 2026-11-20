# HV controller (M6): the most water per watt-hour

The controller chooses the high-voltage level. It runs on the node's ESP32-S3 and **only asks**: it sends
the MCX a level request (`SET_HINT`), and the safety core (M3, on the MCX) decides what is allowed. The
safety core still ramps the level up slowly, backs off on an arc or a warning, and cuts the HV for a person,
an open lid or a fault.

```
MCX telemetry (1 s) ──► ESP32: + load cell, M5b, M5c ──► M6 ──► SET_HINT "m6-<n>" (every 10 s, valid 30 s) ──► MCX safety core ──► HV
                                    arc_risk (M5a) ──┘   └── ARC, ARC_PREDICTED events
```

If the ESP32 stops or M6 is turned off, the last request expires within 30 s, and the MCX goes back to its
default level (`HV_DEFAULT_LEVEL` 0.60). Nothing the controller does can raise the HV above what the safety
core allows.

## How it searches

Nobody knows in advance which level gives the most water per Wh: it depends on the electrode, the gap, the
HV module, the fog and the air, and it drifts as the electrode fouls. So the controller finds it by trying
(perturb and observe, the usual method for finding the top of an unknown curve):

1. It holds a level for 90 s. It judges it on the last 60 s, once the load cell's 30 s rate has caught up.
2. The result is the water collected (the slope of the load cell's mass over those 60 s), **per unit of fog**,
   divided by the node's power: g/Wh. Dividing by the fog index keeps a fog that thickens or thins during a
   step from being mistaken for the effect of the level.
3. It moves one step (0.03) and judges again. While the results are no worse than the best of this sweep
   (within 2 %), it keeps going the same way. Once one is clearly worse, it goes back to the best level,
   stays there for 3 steps, then probes the other side. The best level moves, so it never stops looking.

The power is the HV module's (`p_w`, INA219 on the MCX) plus 1 W for the node itself (both chips and
sensors). Counting the node's own power gives a real optimum: higher levels collect more but cost more.

**It keeps out of the arc zone** with the arc predictor (M5a):

- `arc_risk` above 0.10 (averaged over 5 s): it does not climb. Above 0.20: one step down at once, before
  the MCX has to warn (0.30).
- After an `ARC` or `ARC_PREDICTED` at level L, it stays below L − 0.05. That ceiling rises again by 0.01 a
  minute once there has been no arc for 5 minutes.

**It never goes below the corona onset** (M5c's `health.onset_level`) + 0.08: no corona, no point. It starts
at the MCX's default 0.60, or at the onset + 0.30 if that is higher (a fouled electrode). It keeps its level
between fog episodes.

**Two goals:**

| Goal | Judged on | When |
|---|---|---|
| `efficiency` (default) | grams per Wh of the whole node | the plan's F4; battery or solar nodes |
| `water` | grams per hour | the most water the node can collect without arcing |

**The station still decides when it wants to.** A `SET_HINT` from the station (`python tools/send_cmd.py
--node n01 hint 0.9`) goes to the MCX as before, and M6 waits until it expires. Demo step 4 ("voltage pushed
up on purpose") uses this: the station pushes the level up, the arc predictor warns and the node backs off.
Then M6 takes over again, below the level that arced.

## Results (toy collector: to be measured on the chamber)

There is no HV bench yet, so the test drives the controller against a toy collector, through the **real
safety core** (the same code as the MCX, warning at 0.30). The collector model:

- Townsend corona current I = K·V·(V − V0), lowered by fog.
- Capture driven by the charge: 1 − exp(−I / Ic), as in the Deutsch–Anderson law.
- HV power V·I through a 50 % efficient module, with hv_level 1 = 30 kV.
- Arcs at random, more and more often near a sparkover level that fog lowers.
- Some electrodes foul (V0 rises).
- Each node has its own V0, K, sparkover and voltage scale, because the real HV module is not chosen yet.
- The load cell has 0.05 g of noise.

24 nodes × 4 h:

| | g/Wh | water g/h | arcs/h | warnings/h | faults/h |
|---|---|---|---|---|---|
| fixed 0.50 | 9.26 | 15.6 | 0.6 | 0.0 | 0.01 |
| fixed 0.55 | 10.16 | 18.9 | 2.2 | 0.1 | 0.17 |
| fixed 0.60 (the MCX's default) | 10.47 | 21.6 | 4.6 | 7.7 | 0.56 |
| fixed 0.65 | 10.30 | 23.6 | 8.4 | 16.6 | 1.09 |
| fixed 0.70 | 9.51 | 23.1 | 15.6 | 26.8 | 2.29 |
| **M6, goal efficiency** | **10.58** | 22.4 | **1.5** | 0.0 | **0** |
| **M6, goal water** | 10.39 | **25.5** | 3.6 | 0.2 | 0.01 |

How to read it:

- **On grams per Wh, M6 matches the best fixed level** (+1 % here; between −1.5 % and +1 % on two other
  sets of 24 nodes). It finds that level without being told where it is.
- The g/Wh curve is flat near its top, so a well-chosen fixed level loses little. That is only true if
  someone chose the right level: on the real hardware, nobody knows it yet.
- **It cuts the arcs by two thirds and the faults to zero**, because it keeps out of the arc zone with the
  arc predictor. The fixed levels that collect about as much water (0.60, 0.65) arc 3 to 6 times as often and
  fault about once every one to two hours (each fault stops the node until an operator arms it again).
- **With the goal `water`, it collects more than any fixed level**: 8 % more than the best one (0.65), with
  less than half its arcs.

These are toy numbers. They show the controller works, through the real safety core. They do not show how
much it gains on a real collector. Measure that in the chamber (M11): the same table, with fixed levels
against M6.

## On the boards

**ESP32** (its USB port):

| Command | What |
|---|---|
| `m6` | goal, the level it asks for, the arc ceiling, the last result, the last move and why, requests sent and accepted |
| `m6 off` / `m6 on` | off: no requests, the MCX uses its default level (and the station's hints) |
| `m6 efficiency` / `m6 water` | the goal |

**MCX** (USB terminal): every 10 s, `# station command SET_HINT: accepted`. That is M6's request: the MCX
shows every command it gets, whoever sent it. `hv_level` in the telemetry follows the request, within what
the safety core allows.

**The station does not see M6's requests or the MCX's answers.** The ESP32 counts them (`m6`), so they do
not flood the event log. A refused request is logged on the ESP32's console.

**Try it without hardware:** Wokwi with `tools/mcx_sim.py`, as for M7 (`docs/link.md`). The simulated MCX
follows SET_HINT, so `m6` on the ESP32's console shows the requests going out and being accepted. Wokwi's
load cell is a slider, so it collects no water: the search there shows the requests and the arc limits, not
the optimisation.

## Settings

`ControllerConfig` in `firmware/node/lib/controller/hv_controller.h`:

| Setting | Default | |
|---|---|---|
| `baseW` | 1.0 W | the node's own power, counted in g/Wh: measure it on the real node |
| `dwellMs` / `settleMs` | 90 s / 30 s | how long each level is held, and judged after |
| `step`, `tolerance`, `holdDwells` | 0.03, 2 %, 3 | the search |
| `riskSoft`, `riskHigh` | 0.10, 0.20 | arc_risk: no climbing / one step down (the MCX warns at 0.30) |
| `arcMargin`, `relaxAfterMs` | 0.05, 5 min | below the last arc, and when the ceiling rises again |
| `startLevel`, `aboveOnset` | 0.60, 0.08 | where it starts, and the floor above the corona onset |
| `refreshMs`, `ttlS` | 10 s, 30 s | the request is sent again every 10 s and lasts 30 s |

## Test

```bash
cd firmware/node
pio test -e native -f test_controller        # 11 tests; add -v to see the closed-loop table
```

What they check:

- **The search:** it climbs to the top and stays near it, and goes down when the top is lower. It is not
  fooled by fog thinning during a step, or by a tare in the middle of one.
- **The limits:** it stays below an arc, and the ceiling rises again later. The arc risk stops it climbing
  and brings it down before the MCX warns.
- **The rest:** the station's hint takes over until it expires. Off means no requests. The water goal climbs
  higher than the efficiency goal.
- **The ESP32 side:** the hook reads the JSON telemetry and events. The gateway sends the requests to the MCX
  (topic, cmd_id, ttl, level, time) and keeps their answers off the station without a gap in `seq`. A station
  SET_HINT still reaches the MCX.
- **The closed loop** above, with its assertions: M6's g/Wh within 2 % of the best fixed level, less than
  half the default's arcs, almost no faults, and with the water goal more water than any fixed level.

## Files

| File | What |
|---|---|
| `firmware/node/lib/controller/hv_controller.*` | the search, the arc and risk limits, the requests |
| `firmware/node/lib/controller/controller_hook.*` | M6 on the gateway: telemetry, events and station commands in, requests out |
| `firmware/node/lib/gateway/node_gateway.*` | requests to the MCX as SET_HINT "m6-<n>"; their answers counted, not published; the hook chain |
| `firmware/node/src/link/main_link.cpp` | the controller after the M5 models; the `m6` console command |
| `firmware/node/test/test_controller/test_main.cpp` | the tests and the closed loop |

## Limits to state openly

- **Tested on a toy collector only.** The physics follows the textbook laws, but the numbers are not
  measurements. The gain on g/Wh may be larger or smaller on the real collector: measure it (M11).
- **It needs the load cell.** Each step is judged on the water collected. Without a working load cell the
  results are meaningless: turn M6 off (`m6 off`), and the MCX keeps its default level.
- **It is slow on purpose.** One step every 90 s, because the load cell needs that long to see a difference.
  It takes 5 to 10 minutes to find the top after a big change.
- **It searches one node at a time.** The station's view across nodes (M8) does not feed into it yet.
- `baseW` (1 W) is an estimate. Measure the node's own consumption once it is built.