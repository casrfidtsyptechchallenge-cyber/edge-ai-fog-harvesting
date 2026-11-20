"""M4, station side: the list of RFID badges, and every node's copy of it.

The station owns the badge list, in one file (data/badges.json). Each node keeps a copy, sent with the
SYNC_ALLOWLIST command, so badges keep working while the Pi or the radio is down.

    python -m station.badges add 04A1B2C3 operator --label "white card"
    python -m station.badges list
    python -m station.badges sync --node all
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "contract"))  # makes `import aeroion_contract` work, as in tools/