# Training pipeline (M11): from the recorded runs to the firmware

One command takes every run recorded with M2 (`data/runs`) and does the rest:

```bash
python -m ml pipeline
```

| Step | What it does |
|---|---|
| 1. Datasets | builds each run's tables again if its `raw.jsonl` changed (as `python -m station.datalog build`) |
| 2. Arc predictor (M5a) | retrains on the recorded windows, tests on the newest runs, compares with the model in the firmware, and writes the firmware's headers **only if the new model is better** |
| 3. Virtual fog (M5b), electrode health (M5c) | checks them on the runs: the virtual fog against the optical sensor, the onset and score per run |
| 4. HV controller (M6) | water, grams per Wh, arcs and faults per hour for each value of a run setting (`controller=m6`, `controller=fixed-0.60`) |
| 5. Report | `data/reports/pipeline-<date>-<time>.md`, also printed |

Nothing runs on the station or the nodes: this is the laptop (numpy, `requirements-ml.txt`).

## Record the data (on the bench)

The pipeline learns from what M2 records. In the chamber, with the node armed and the fog on:

```bash
python -m station.datalog record --tag gap15 --set gap_mm=15 --set controller=m6       # Ctrl-C to stop
```

- **Windows for the arc predictor:** on the MCX, `waves 2` (a window every 2 s while the HV is allowed).
  The windows the node sends on its own also count: `ARC_PREDICTED`, and `ARC`, which is kept for the record
  but not trained on, because it holds the arc itself.
  Each window's label is whether a real `ARC` event came within 5 s (`arc_followed` in `windows.csv`).
- **For the controller table:** record runs with `--set controller=m6` (M6 on), and others with
  `--set controller=fixed-0.60` (`m6 off` on the ESP32: the MCX holds its default 0.60). Use the same fog,
  the same electrode, about the same time each.
- More runs, and longer ones, make better models. The test takes the newest quarter of the runs, so record
  at least 4.

## What decides that a new model goes into the firmware

- The new model is tested on the **newest runs**, which it never trained on. With a single run, it is
  tested on that run's last quarter.
- The model in the firmware (named in `arc_model_data.h`, kept in `ml/models/`) is tested on the same
  windows.
- The headers (`arc_model_data.h`, `arc_model_test.h`) are written only if the new model's **log loss** is
  lower. The log loss is used because `arc_risk` is read as a probability (the MCX warns at 0.30): a model
  that ranks windows well but gives wrong probabilities would warn too often or too late.
- At least 50 test windows are needed, with at least 5 followed by an arc and 5 not (`--min-test`). Below
  that it does not judge, and the firmware keeps its model.
- `--force` writes the headers anyway; `--dry-run` writes nothing into the firmware.

**Training data:** the recorded windows, plus 30 synthetic runs (`ml/arc_physics.py`). The synthetic runs
teach the general shape, so a few hours of chamber data adapt the model rather than build it from nothing.
The recorded windows are repeated so they weigh as much as the synthetic ones together, at most 20 times.
`--synthetic-runs 0` trains on the recorded windows alone, once there are many.

**After the headers are written:**

```bash
cd firmware/node && pio test -e native -f test_edge_ai     # the golden vectors: the MCX gives the same integers
```

Then build and flash the MCX (`docs/mcx_node.md`). The ESP32 does not change: M5b and M5c learn on the node.

Every model trained is kept in `ml/models/` (weights, 8-bit version, rule, scores, decision, the runs used):

```bash
python -m ml models                                   # the versions, their test scores, the one in the firmware
python -m ml models --deploy arc-cnn-sim-20261116-s1  # put an earlier version back into the firmware's headers
```

## Try it today (no chamber yet)

```bash
python -m ml demo-runs                               # 4 stand-in runs of 30 min in data/demo-runs, 5 s
python -m ml pipeline --data data/demo-runs --dry-run    # 30 s; --dry-run: the firmware is not changed
```

**The stand-in runs are not measurements.** Each one combines:

- the node simulator's telemetry and events (`sim/fake_node.py`, with the HV held at 0.55 or 0.70);
- a current window every 6 s from the arc physics (`ml/arc_physics.py`), each followed by an `ARC` when the
  physics says so.

They show the whole path working: run folders, datasets, retraining, the comparison, headers that pass the
firmware's golden test, the report. The report says when stand-in runs are in it.

On them (1192 windows, 325 followed by an arc; tested on the newest run's 298 windows):

| | AUC | log loss | arc windows warned at 0.30 | safe windows warned |
|---|---|---|---|---|
| new CNN, 8 bits | 0.948 | 0.254 | 83 % | 7.8 % |
| new rule-based fallback | 0.933 | 0.297 | 80 % | 11.5 % |

These are the same numbers as on the synthetic test runs (`docs/edge_ai.md`), as they should be: the
windows come from the same physics. In the controller table of the stand-in runs, the arcs per hour come
from the stand-in windows, whatever the level: they mean nothing.

## The report

```
# Training pipeline (M11), <date>
## Arc predictor (M5a)      windows used and skipped; new model, firmware's model, rule-based fallbacks on the
                            same test windows; the decision; whether the headers were written
## Virtual fog sensor (M5b) and electrode health (M5c)
                            per run and node: how often fog_idx_virt was given, its error against fog_idx_opt
                            and its bias; the onset, first and last; the last health score
## HV controller (M6)       per value of the run setting (--compare-by, default controller): hours of fog,
                            mean fog, water g/h, g/Wh of HV energy, arcs/h, warnings/h, faults per run hour
```

## Intervention bias: what the arc labels can and cannot say

Every `ARC_PREDICTED` makes the safety core back off. If no arc follows, that proves nothing: the back-off may
have prevented it. A model retrained with those windows labelled "no arc" learns that its warnings were wrong
and warns less: the better the predictor works, the more it would unlearn. So:

- M2 writes `intervened` in `windows.csv`: 1 when an `ARC_PREDICTED` came within the horizon after the window
  (always for `pre_arc` windows). M11 leaves these windows out ("a back-off followed (outcome unknown)").
- Best training data: periodic windows (`waves 2`) while the predictor is in **`ai observe`** on the MCX: it
  computes and reports its risk, the safety core does not act on it (the 250 µA arc detection and its back-off
  stay on). For supervised chamber campaigns only; `ai act` goes back to normal.

## On-node learning (experiment, not in the firmware): `python -m ml adapt`

Could each node fit the CNN's last layer (33 numbers) to its own arcs, and the station average them (federated
learning)? `ml/arc_adapt.py` tries it on simulated nodes in a closed loop with the safety core's rules. What the
experiment showed, step by step:

| Version | What happened |
|---|---|
| labels from everything, censoring the windows followed by a back-off | **harmful**: the kept windows are the ones the model found safe (true arc probability 0.004 against 0.011 overall); it learns that all is safe, climbs, arcs: water ÷3, faults ×4 |
| + windows before every arc | **harmful the other way**: arcs are 38 % of the labels instead of ~8 %, it warns 3 to 5 times too often |
| **only probe windows** (60 s every 10 min the node does not act on the CNN, arc detection on), Newton fit pulled to the factory weights, trust region (mean logit +2 / −0.5 per round), blocked cross-validation gate (gain > 2 standard errors) | safe, see below |

Final protocol, 5 unseen seeds, 3 nodes, 2 h of learning, 30 min of judging (paired with the factory layer):

| Node | Version | Water | Faults/h | Arcs/h | Log loss vs truth |
|---|---|---|---|---|---|
| A: pulses 3× smaller (the case the novelty guard misses) | local | +4.6 % (3 better, 2 worse) | **−3.6** | **−27** | **−0.090** |
| A | federated | +8.1 % | −1.6 | −8 | −0.036 |
| B: current × 0.3, noise × 2 | local | +15 % (one seed adopted) | 0 | +4 | −0.003 |
| C: like the training data | local / federated | +1 % / +9 % | 0 / 0 | −4 / +2 | −0.03 / −0.04 |

Reading: never more faults on average; on the shifted node, far fewer arcs and faults and a better calibration;
the water effect varies from seed to seed. Not robust enough to switch on in the firmware. What it gives today:
the intervention-bias fix above and `ai observe`. Next: the same protocol on chamber runs.

## Fog nowcast from real METAR data: `python -m ml fog-forecast`

The first model trained on real data. Download from the IEM archive (network "Tunisia ASOS", no account):
`tmpc, dwpc, relh, sknt, drct, vsby`, UTC, comma-delimited, several years. Then
`python -m ml fog-forecast data/metar/*.csv`. Target: fog (visibility < 1 km) starts within 1 h / 3 h while
there is none now. Features: what a node measures (temperature, dew point depression from the SHT45, their
1 h / 3 h changes, hour, day of year); a second model adds the wind to price an anemometer. Logistic regression,
trained on every year but the last, tested on the last whole year, against climatology and the
"spread ≤ 1 °C and wind ≤ 3 kt" rule of thumb. Tests (`tests/test_fog_forecast.py`) check the reading, the
dew point, that no feature looks ahead, and the split: they use synthetic reports, so **no result is claimed
until the real files are run**.

## Test

```bash
python -m pytest -q tests/test_ml_pipeline.py      # 6 tests, about 10 s (skipped without numpy)
```

What they check:

- **The stand-in runs:** every message is valid against the contract, with no gap in `seq`. The windows are
  labelled, and the test takes the newest run only.
- **The pipeline:** it retrains and writes headers that are bit-exact with the saved model. Its golden
  windows go from safe to risky.
- **The decision:** it compares with the model in the firmware. It leaves the firmware alone with
  `--dry-run`, with too few test windows, or when the new model is not better. `--force` writes anyway.
- **The other steps:** with no windows it trains nothing but still reports. The controller table groups by
  the run setting. The virtual fog check works. `models` marks the model in the firmware, and
  `--deploy` puts an earlier one back.

## Files

| File | What |
|---|---|
| `ml/pipeline.py` | the pipeline: datasets, retraining and decision, node models check, controller table, report |
| `ml/demo_runs.py` | the stand-in runs |
| `ml/__main__.py` | `pipeline`, `models` (with `--deploy`) and `demo-runs`, next to `arc-train` and `arc-eval` |
| `tests/test_ml_pipeline.py` | the tests |

## Limits to state openly

- **No chamber data yet.** The pipeline has only run on stand-in runs. The first real runs will tell
  whether the CNN still beats its rule-based fallback, and what warning threshold to use. Run
  `python -m ml arc-eval` again with the new model, and look at the controller table.
- **The labels depend on the ARC events.** An arc the MCX misses (shorter than its window, between windows)
  is not a positive label. A false ARC (a spike from interference) is. Look at `windows.csv` before trusting
  a big change.
- **The controller table compares runs, not paired trials.** Fog, electrode and air differ between runs,
  and the table shows the mean fog so like can be compared with like. Several runs of each kind are needed
  before a difference means anything.
- 5b and 5c are checked, not retrained: they learn on each node. Their calibration from the chamber (the
  `setCalibration` of the virtual fog sensor) is not written to the ESP32 yet.