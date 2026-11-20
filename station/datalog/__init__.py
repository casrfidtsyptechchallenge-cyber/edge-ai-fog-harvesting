"""M2: data logging and labelling.

Records contract messages into "runs" (one folder per experiment), then turns each run into labelled CSV
tables: training data for the edge AI (M11 -> M5) and per-fog-episode measurements for the report.

    python -m station.datalog record --tag gap15 --set gap_mm=15 --set hv_mode=ai
    python -m station.datalog import run.jsonl --tag sim
    python -m station.datalog build latest
    python -m station.datalog list
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "contract"))  # makes `import aeroion_contract` work, as in tools/