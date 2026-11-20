# Datasets (M2 output, M11 input)

One folder per experiment: `data/runs/<run_id>/`

| File | Written by | Content |
|---|---|---|
| `run.json` | record / import | run id, date, contract version, settings (`--set key=value`), notes |
| `raw.jsonl` | record / import | every message as received; never edited |
| `label_config.json` | you (optional) | overrides label thresholds for this run only |
| `dataset/*.csv`, `summary.json` | build | rebuilt from `raw.jsonl` at any time |

Empty cell = value missing (null). Booleans are 1/0. Times are Unix ms.

## Tables

| Table | One row per | Used for |
|---|---|---|
| `telemetry.csv` | node, second | every contract field (health flattened to `health_*`) + labels |
| `events.csv` | event | `data` column is compact JSON |
| `windows.csv` | current window (`wave`) | arc predictor (M5a): `samples` is space-separated µA |
| `segments.csv` | fog episode, node | report numbers: g collected, g/Wh, arcs/min |

## Labels (telemetry.csv)

| Column | Meaning | Target of |
|---|---|---|
| `label_fog_class` | none / light / dense from `fog_idx_opt` | virtual fog sensor (M5b) |
| `label_rate_gpm` | slope of `mass_g` over a centred window, ignoring tare drops | yield model (M6) |
| `label_arc_next` | 1 if an ARC event follows within `arc_horizon_s` | arc predictor (M5a) |
| `exclude_reason` | "" = usable; else `state`, `person_near` | filter before training |

`windows.csv` label: `arc` (trigger ARC), `pre_arc` (ARC_PREDICTED), `normal` (MANUAL), `novel` (NOVEL);
`arc_followed` says whether a real arc came within the horizon; `intervened` = 1 when the safety core backed off
on a prediction within the horizon (always for `pre_arc`). Then `arc_followed = 0` proves nothing: the back-off
may have prevented the arc. M11 leaves those windows out (intervention bias). Best training data: periodic
windows (`waves 2`) recorded with the predictor in `ai observe` (it computes, the safety core does not act on it).

Thresholds: `DEFAULT_CONFIG` in `station/datalog/labels.py`. Simulator runs only exercise the pipeline;
models are trained on chamber runs.# Datasets (M2 output, M11 input)

One folder per experiment: `data/runs/<run_id>/`

| File | Written by | Content |
|---|---|---|
| `run.json` | record / import | run id, date, contract version, settings (`--set key=value`), notes |
| `raw.jsonl` | record / import | every message as received; never edited |
| `label_config.json` | you (optional) | overrides label thresholds for this run only |
| `dataset/*.csv`, `summary.json` | build | rebuilt from `raw.jsonl` at any time |

Empty cell = value missing (null). Booleans are 1/0. Times are Unix ms.

## Tables

| Table | One row per | Used for |
|---|---|---|
| `telemetry.csv` | node, second | every contract field (health flattened to `health_*`) + labels |
| `events.csv` | event | `data` column is compact JSON |
| `windows.csv` | current window (`wave`) | arc predictor (M5a): `samples` is space-separated µA |
| `segments.csv` | fog episode, node | report numbers: g collected, g/Wh, arcs/min |

## Labels (telemetry.csv)

| Column | Meaning | Target of |
|---|---|---|
| `label_fog_class` | none / light / dense from `fog_idx_opt` | virtual fog sensor (M5b) |
| `label_rate_gpm` | slope of `mass_g` over a centred window, ignoring tare drops | yield model (M6) |
| `label_arc_next` | 1 if an ARC event follows within `arc_horizon_s` | arc predictor (M5a) |
| `exclude_reason` | "" = usable; else `state`, `person_near` | filter before training |

`windows.csv` label: `arc` (trigger ARC), `pre_arc` (ARC_PREDICTED), `normal` (MANUAL); `arc_followed` says
whether a real arc came within the horizon.

Thresholds: `DEFAULT_CONFIG` in `station/datalog/labels.py`. Simulator runs only exercise the pipeline;
models are trained on chamber runs.