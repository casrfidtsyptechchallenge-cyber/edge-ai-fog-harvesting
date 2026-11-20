"""M8: network intelligence on the station.

The fog-front tracker: from the times at which the nodes detect fog and where they stand, it estimates the
direction and speed of the fog front, publishes them for the dashboard (aeroion/station/front) and pre-arms
the nodes the fog has not reached yet (PREARM), so they start collecting as soon as it arrives.

    python -m station.network                         live, on the broker of this computer
    python -m station.network --prior 90 1.0          the site's usual fog: moving toward 90° (east) at 1 m/s
    python -m station.network score run.jsonl         how good the tracker is on a simulator run (--truth)

docs/network.md has the details.
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "contract"))  # makes `import aeroion_contract` work, as in tools/