"""M10: the station dashboard.

A web page served by the Raspberry Pi on its own hotspot (no internet): live state of every node, fog,
current, water, electrode health, the node map with the fog front, alerts, and a jury view.

    python -m station.dashboard                          live, from the MQTT broker on this computer
    python -m station.dashboard --replay run.jsonl       a recorded run (no broker needed)

Then open http://localhost:8080. docs/dashboard.md has the details.
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "contract"))  # makes `import aeroion_contract` work, as in tools/