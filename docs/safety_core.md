# Safety core (M3)

The node state machine decides when the high voltage (HV) may be on, and how high. It is plain logic
in `firmware/node/lib/safety/`, tested on the PC (`test/test_safety`). AI modules and the station can
only make requests. Every change is published as a `STATE_CHANGE` event with its reason.

| State | HV | Meaning |
|---|---|---|
| `LOCKED` | off | nobody authorised the node; the state after every power-up or restart |
| `ARMED` | off, allowed | an operator badge authorised it; waiting for fog |
| `HARVEST` | on | fog present; level ramps up to the request, never above 0.95 |
| `BACKOFF` | reduced | an arc was predicted or happened; level cut by 0.12 for 4 s |
| `SAFE` | off | person near, or a fault |
| `MAINTENANCE` | off, locked | technician lock-out |

## Transitions

Checked every 50 ms, in this order (the first that applies wins):

| From | Trigger | To |
|---|---|---|
| ARMED, HARVEST, BACKOFF | fault: lid open; radar, lid switch or collector current not reporting | SAFE (latched) + `FAULT` |
| ARMED, HARVEST, BACKOFF | radar sees a person | SAFE (presence) |
| SAFE (presence) | a fault appears | SAFE (latched) + `FAULT` |
| SAFE (presence) | nobody seen for 10 s (`presenceClearMs`) | ARMED |
| HARVEST, BACKOFF | arc (current > 250 µA) | BACKOFF + `ARC` |
| HARVEST, BACKOFF | 3 arcs within 60 s, or current > 250 µA for 1 s | SAFE (latched) + `FAULT HV_OVERCURRENT` |
| HARVEST, BACKOFF | level ≥ 0.45 and current < `minCurrentUa` (0.3 µA) for 3 s: a disconnected sensor reads ~0, it does not stop reporting. On once the HV stage exists (`AEROION_HAS_HV_OUTPUT`) | SAFE (latched) + `FAULT SENSOR_FAIL` |
| ARMED | fog detected | HARVEST |
| HARVEST | fog cleared | ARMED |
| HARVEST | arc_risk > `arcWarnRisk` (M5a): 0.30 on the MCX, where arc_risk is the probability of an arc within 5 s (library default 0.70) | BACKOFF + `ARC_PREDICTED` |
| BACKOFF | 4 s passed and arc_risk ≤ `arcWarnRisk` | HARVEST, or ARMED if the fog is gone |

## Badges (every tap is logged as `BADGE_TAP`)

| Badge | State | Result |
|---|---|---|
| operator or admin | LOCKED | ARMED, unless a fault is present (denied) |
| operator or admin | ARMED, HARVEST, BACKOFF | LOCKED (disarm) |
| operator or admin | SAFE (presence) | LOCKED (disarm): the operator walked up to stop it |
| operator or admin | SAFE (latched) | ARMED once the cause is gone, else denied |
| operator or admin | MAINTENANCE | denied |
| technician | any except MAINTENANCE | MAINTENANCE + `MAINT_ON` |
| technician | MAINTENANCE | LOCKED + `MAINT_OFF`, only with the badge that locked it |
| household, unknown | any | denied |

## Station commands

| Command | Result |
|---|---|
| `LOCK` | LOCKED from any state; rejected in MAINTENANCE (only the technician releases a lock-out; HV is off anyway) |
| `PREARM` | only in ARMED: fog threshold 0.30 → 0.20 for eta_s + 60 s |

## Changes from the architecture document

- A technician badge works in SAFE too: the radar always sees the technician walking up, so SAFE is
  the normal state when they tap.
- An operator tap in SAFE (presence) disarms, for the same reason.
- A station LOCK cannot end a maintenance lock-out.
- BACKOFF also waits for the arc risk to fall, not only for the timer.

## Limits to state openly

- The 10 Hz current reading catches sustained overcurrent and long discharges. Short arcs (µs to ms)
  need the fast current windows of M5a, which report them with `SafetyCore::reportArc()`.
- Software is the second layer. The lid switch should also cut the HV supply in hardware, and a
  pull-down keeps the HV MOSFET off while the ESP32 boots, restarts or hangs.
- A watchdog restarts a hung ESP32 within 3 s; it comes back LOCKED and reports `FAULT WATCHDOG`.