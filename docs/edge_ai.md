# Edge AI (M5): arc predictor, virtual fog sensor, electrode health

Three models run on the node itself, so they keep working when the station or the radio is down:

| Part | Runs on | Input | Output (telemetry) | Feature |
|---|---|---|---|---|
| **5a arc predictor** | MCX A156 | 25.6 ms of collector current at 10 kHz, every 100 ms | `arc_risk`, and the back-off | F5 (core) |
| **5b virtual fog sensor** | ESP32-S3 | `i_ua`, `hv_level`, `fog_idx_opt` (1 s) | `fog_idx_virt` | F8 |
| **5c electrode health** | ESP32-S3 | `i_ua`, `hv_level` (1 s) | `health.onset_level`, `k_coef`, `score` | F9 |

```
MCX:  fast ADC (A0) ──► window ──► 8-bit CNN (or rule fallback) ──► arc_risk ──► safety core: above 0.30, ARC_PREDICTED + BACKOFF
                           └──► peak above 250 µA: an ARC too short for the 10 Hz reading
                           └──► "wave" message on a warning, an arc, or on demand (training data)
ESP32: MCX telemetry ──► Townsend fit (onset V0, K) ──► health score
                    └──► k = I / (V (V − V0)) vs optical fog ──► fog_idx_virt (works when the optical sensor does not)
```

The C++ is in `firmware/node/lib/edge_ai` (portable, tested on the PC); the training is in `ml/` (numpy, on the
laptop). `python -m ml pipeline` retrains the arc predictor on the runs recorded in the chamber (M11,
`docs/training.md`).

## 5a: the arc predictor

**What it predicts:** the probability that an arc happens within the next 5 s if the HV stays where it is
(the same 5 s as M2's `label_arc_next` and the safety core's "predicted" window). The contract calls
`arc_risk` a probability, and it is one: when the model says 0.7, arcs followed 69 % of the time on unseen runs.

**The model:** a 1D-CNN, 8 bits, about 750 weights and 46 000 multiply-adds per window.

| Step | Detail |
|---|---|
| prepare | sort the window: median and noise (median − 16th percentile); z = (x − median) / max(median, 2 µA), clipped to ±1, as int8 |
| conv 1 | 1 → 8 channels, kernel 7, stride 2, ReLU (125 steps) |
| conv 2 | 8 → 16 channels, kernel 5, stride 2, ReLU (61 steps) |
| pool | average and maximum over time: 32 numbers |
| dense | 32 → 1, sigmoid |

Weights int8 (one scale per layer), activations int8, integer multiplier and shift between layers: the MCX
and the Python reference give exactly the same integers (golden vectors in the firmware tests).

**The rule-based fallback** (the plan asks for one): pre-discharge pulses per second (2 samples or more, 5
noise sigmas above the median), the noise and the current, through a logistic regression whose 4 numbers are
fitted on the same data. It runs alongside the CNN and takes over when the window is not what the CNN was
trained on (another length, or a sample rate more than 10 % off), or when chosen with `ai rules`.

**Trained on synthetic windows, for now.** There is no HV bench yet, so `ml/arc_physics.py` stands in. It
draws a baseline current from the Townsend law, lowered by fog, with noise, mains ripple and interference
spikes. As the level nears sparkover, short pre-discharge pulses come more often and grow, and arcs follow
at a rate that rises the same way over a narrower band. Each "run" draws its own electrode, noise and
sparkover level.

### Results (synthetic: to be repeated on chamber data)

Per window, on 25 runs never seen in training (10 000 windows, 25 % followed by an arc within 5 s):

| | ROC AUC | Log loss | Arc windows warned at 0.30 | Safe windows warned |
|---|---|---|---|---|
| **CNN, 8 bits (as on the MCX)** | **0.948** | 0.246 | 86 % | 10.8 % |
| CNN, float | 0.948 | 0.246 | 86 % | 10.9 % |
| rule-based fallback | 0.932 | 0.278 | 82 % | 12.0 % |
| best possible (the true probability) | 0.970 | 0.189 | 89 % | 9.1 % |

The arcs are random: even knowing the true probability, no predictor can do better than 0.970. The 8 bits
cost nothing measurable.

**Closed loop** (`python -m ml arc-eval`): 8 unseen runs of 30 min of fog. The controller keeps pushing the
HV up, as M6 will, to collect more. The safety core's rules apply: back-off after a warning or an arc, and a
FAULT after 3 arcs within a minute, which stops the node until an operator arms it again (taken as 5 min).

| | Arcs/h | Warnings/h | Faults/h | Water |
|---|---|---|---|---|
| no predictor | 28.5 | 0 | 8.2 | 100 % |
| CNN, warn at 0.7 | 34.0 | 39 | 4.8 | 196 % |
| CNN, warn at 0.5 | 25.2 | 75 | 2.8 | 241 % |
| **CNN, warn at 0.3** | **11.2** | 160 | **0.2** | **296 %** |
| rule-based, warn at 0.3 | 9.2 | 114 | 0.2 | 301 % |

Without the predictor the node arcs its way into a fault eight times an hour and stands idle 40 minutes of
every hour. Warning at 0.30 nearly removes the faults and triples the water. That is why the MCX warns at
`ARC_WARN_RISK 0.30f` (`mcx_node_config.h`), not at the safety core's default 0.70. On these synthetic
runs the fallback does as well as the CNN: the synthetic physics is simple enough for three hand-picked
inputs. Whether the CNN earns its place on real current windows is the first thing to check on the bench (M11).

**Latency:** measured on the board with the `ai` command (capture time and inference time). Expected: a few
milliseconds for the 46 000 multiply-adds at 96 MHz. The window itself takes 25.6 ms of every 100 ms of the
main loop, which also runs the safety core every 50 ms.

### Train it again

On the laptop, from the project folder:

```bash
python -m pip install -r requirements-ml.txt       # numpy
python -m ml arc-train                             # 35 s: writes the two headers and ml/models/<name>.npz
python -m ml arc-eval                              # 2.5 min: the closed-loop table above
```

`arc-train` writes `firmware/node/lib/edge_ai/arc_model_data.h` (the model the MCX runs) and
`firmware/node/test/test_edge_ai/arc_model_test.h` (golden vectors), and keeps the model in `ml/models/`.
Then rebuild the MCX firmware. Options: `--train-runs`, `--epochs`, `--seed`, `--threshold`; see `--help`.

## 5a+: the novelty guard ("the AI that knows when it does not know")

The CNN was trained on synthetic windows. On the real HV stage it will meet windows unlike anything it saw. Its
risk is then a guess, and nothing in the risk itself says so. The guard says so, for every window, from numbers
the firmware already computes (no extra capture, about 600 multiply-adds):

| Check (first that applies) | Catches | How |
|---|---|---|
| saturated | input clipped (amplifier gain too high) | 4 samples or more equal to the window's maximum |
| out of range | another current scale, a dead or disconnected sensor | log(1 + pulses/s), log(noise), log(median µA) outside the training range (+ margin) |
| unusual shape | more noise, mains ripple, other shapes | Mahalanobis distance of the CNN's 32 pooled features, log(1 + f), Ledoit-Wolf covariance |

The CNN scales each window by its median, so its features alone cannot see a change of current scale or a dead
sensor: that is why the three physical inputs are checked separately. Thresholds are set on validation runs so
that **1 % of normal windows** are called novel.

**What the node does with a novel window.** It keeps the CNN's risk, sends the window as a `wave` with trigger
`NOVEL` (at most one every 10 s: training material for M11), and reports `arc_novel` in the telemetry: the share
of the last 50 windows (5 s) that were novel. Above 0.20 the HV controller (M6) stops climbing: when the
predictor is unsure, do not explore towards the arc zone. The MCX's own arc detection (250 µA) is unchanged.

**Why not switch to the rule-based fallback?** That was the first design. The evaluation below rejected it: the
rule is fitted on the same synthetic data and extrapolates worse. With 3× the noise, switching raised the log
loss from 0.36 to 0.60.

### Results (`python -m ml novelty`, synthetic: 20 unseen runs per condition, 4000 windows each)

| Condition (what could differ on the bench) | Windows called novel | CNN AUC | Rule AUC | Why |
|---|---|---|---|---|
| in distribution (unseen runs) | **1.0 %** | 0.942 | 0.920 | as designed |
| current × 0.1 (weak ion module) | **51 %** | 0.856 | 0.816 | range 12 %, shape 39 % |
| noise × 3 (long unshielded wires) | **35 %** | 0.879 | 0.850 | shape |
| mains ripple × 5 (cheap 12 V adapter) | 17 % | 0.915 | 0.846 | shape |
| interference × 10 (PWM nearby) | 4 % | 0.935 | 0.926 | harmless: little flagged |
| pulses 3× smaller (other electrode) | 0.6 % | 0.854 | 0.846 | **not seen**: see below |
| ADC clipping at 60 µA | 21 % (= the clipped windows) | 0.723 | 0.804 | saturated |
| +15 µA offset (zero not calibrated) | 4 % | 0.951 | 0.942 | harmless: little flagged |
| sensor disconnected | **100 %** | 0.491 | 0.491 | out of range |

Limits: the guard catches gross changes, not subtle ones. Pulses 3× smaller cost the CNN 0.09 of AUC and are
almost never flagged. Only retraining on the node's own windows fixes that (M11, and on-device adaptation).
The C++ (`lib/edge_ai/arc_novelty.cpp`) gives the same reason as the Python reference on 10 000 windows
(100 %, distance within 7·10⁻⁶ relative). `arc_novelty_data.h` is written with the model's headers by
`arc-train`, `pipeline` and `models --deploy`; a firmware test checks that both name the same model.

## 5b: the virtual fog sensor

Fog droplets capture ions, so in fog the corona current is lower than the same voltage gives in clear air.
With the onset V0 from 5c, each reading gives the momentary Townsend coefficient k = I / (V·(V − V0)), and
k = K·(1 − s·fog). While the optical sensor works, the ESP32 learns K and s from it (a least-squares line,
slowly forgetting, about half an hour of memory); then fog = (1 − k/K) / s comes from the current alone,
and keeps coming when the optical sensor is fogged up, dirty or broken: the collector is also the sensor.

Null while the HV is off, before V0 is known, before the fog has varied enough to learn from, or when V
is within 0.05 of V0 (too noisy there).

## 5c: electrode health

The Townsend law of corona, I = K·V·(V − V0), is a straight line when written I/V = K·V − K·V0. The ESP32
fits it by least squares on the readings of the last minute, once the level has moved at least 0.10: a ramp
up, a back-off, the controller's steps. Fog lowers I by a factor, which changes K but not V0, so the onset
is measured whatever the fog. Once 5b knows how much fog lowers the current, the fit gets the current as it
would be in clear air: a fog thickening during a sweep no longer tilts it.

A dirty or worn electrode needs more voltage to start the corona: V0 rises. The score compares V0 with the
clean electrode's: 1 = like new, 0 = risen by 0.15 (clean it). The dashboard shows it on each node card, and
in "Needs attention" below 0.5.

**The clean reference** is the mean onset of the first 3 fits. The ESP32 keeps it in flash, so it survives
a restart. It is learned again when the electrode has been serviced: at the end of a maintenance lock-out
(MAINT_OFF, the technician's badge), or with the ESP32's console command `clean`.

## On the boards

**MCX** (USB terminal, 115200 baud):

| Command | What |
|---|---|
| `ai` | model, mode, threshold; last window's risk (CNN and rule), median, peak, pulses/s; sample rate, capture and inference time |
| `ai auto` / `ai model` / `ai rules` | who decides: the CNN with the rule as fallback (default), the CNN only, the rule only |
| `wave` | the next window goes out as a `wave` message (`MANUAL`) |
| `waves 5` / `waves off` | a window every 5 s while the HV is allowed (training data for M11, recorded by M2) |

**ESP32** (its USB port): `health` (fits, onset, K, score, reference; the virtual fog's calibration) and
`clean` (the electrode was cleaned).

**Bench test of 5a without HV:** a potentiometer on A0 (0 to 3.3 V). First turn it to 0 V and type `zero`
(the current's zero is A0's voltage with the HV off; then 1 mV on A0 reads as 0.1 µA). Then arm the node
(SW2) with fog on (the fog knob): windows are taken while the HV is allowed.

1. `ai`: windows every 100 ms, a sample rate near 10 000 Hz, the inference time in µs. Write it down for the
   report ("latency measured on device").
2. A steady voltage on A0, below 2.5 V (250 µA): a low risk.
3. Tap A0 with a wire to 3V3 for a moment (330 µA): the window's peak shows the arc, `ARC` in the events,
   and a wave message with the trigger `ARC`. Three taps within a minute give a FAULT, as designed: arm
   again with SW2.
4. `waves 2` while M2 records (`python -m station.datalog record --duration 60`, through the ESP32 or
   `tools/serial_bridge.py`), then `python -m station.datalog build latest`: the run's `windows.csv` has a
   row for each `MANUAL` window.

A steady potentiometer gives a low risk (about 0.02 with the synthetic model, at any current): it has no
pre-discharge pulses, so a real warning needs the HV module and a collector. To see the warning path anyway,
pull the wire off A0: a floating input usually reads as noise, which the model takes for a noisy discharge
(random 0 to 100 µA gives 0.75): `ARC_PREDICTED`, `BACKOFF`, and a wave message with that trigger. Put the
wire back before you go on.

## Test

```bash
python -m pytest -q tests/test_ml_arc.py          # 7 tests (skipped without numpy)
cd firmware/node && pio test -e native -f test_edge_ai     # 15 tests
```

PC side: the synthetic physics, pulse counting, the integer rescaling, training and 8 bits, the AUC, the
closed loop, and that the firmware's headers match the saved model. Firmware side: the window prepared and
the int8 network bit for bit as in Python, the fallback and its modes, the peak of a short arc, the
Townsend fit (with fog, noise, no sweep, a fouled electrode, the reference), the virtual fog (learning,
working alone, refusing without what it needs), the ESP32's models on JSON telemetry and through the
gateway (a fog front during the sweeps, MAINT_OFF), the arc guard, and the wave message.

## Files

| File | What |
|---|---|
| `firmware/node/lib/edge_ai/arc_predictor.*` | 5a: preparation, int8 CNN, rule fallback |
| `firmware/node/lib/edge_ai/arc_model_data.h` | the CNN's weights and the rule's numbers (GENERATED by `python -m ml arc-train`) |
| `firmware/node/lib/edge_ai/arc_guard.*` | 5a at work: a window every 100 ms while the HV is allowed; the wave message |
| `firmware/node/lib/edge_ai/electrode_health.*` | 5c: Townsend fit, onset, score |
| `firmware/node/lib/edge_ai/virtual_fog.*` | 5b: calibration from the optical sensor, fog from the current |
| `firmware/node/lib/edge_ai/esp_models.*` | 5b and 5c on the MCX's telemetry, in the ESP32's gateway |
| `firmware/mcx_node/source/main.cpp`, `mcx_io.c` | the fast window on A0, the arc guard in the safety loop, `ai` / `wave` |
| `firmware/node/src/link/main_link.cpp` | the models in the gateway, the reference in flash, `health` / `clean` |
| `ml/arc_physics.py`, `ml/arc_cnn.py`, `ml/arc_eval.py`, `ml/__main__.py` | synthetic windows, training, 8 bits, export, evaluation |

## Limits to state openly

- **The arc predictor is trained on synthetic windows.** Its numbers say the pipeline works end to end (data,
  training, 8 bits, the MCX, the back-off), not how well it predicts real arcs. Record windows on the HV bench
  (`waves 2`, ARC events) and retrain (M11) before quoting accuracy.
- The window covers 25.6 ms of every 100 ms: a very short arc is caught about one time in four (the 10 Hz
  reading catches longer ones).
- A wave message is about 1.5 kB on each serial port. When a telemetry line is still going out, the MCX's
  send buffer is full for a moment and the main loop waits, up to about 60 ms: one safety cycle late, once
  per wave message (after the back-off, which is already applied).
- No HV output on the MCX yet: `hv_level` is the level the safety core allows. 5b and 5c need the real current
  to mean anything.
- 5c needs the level to move (0.10 within a minute): with a fixed level, no new fit. 5b needs the optical
  sensor to have seen the fog vary before it can stand in for it.
- The health reference is learned from the first fits after a cleaning: if the electrode was not clean then,
  the score is relative to that state.
- Before 5b is calibrated, a fog changing during a sweep can move one fit's onset by a few hundredths; the
  average over the following fits corrects it.