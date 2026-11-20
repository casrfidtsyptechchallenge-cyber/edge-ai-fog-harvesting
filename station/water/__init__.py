"""M9: the station's water point.

Household cards draw a daily quota of water, but only while the water is safe (TDS and pH) and the filter
cartridge is genuine and not used up. Every dispense, refusal and change is kept in a ledger on the Pi
(data/water.db), so nothing is lost while the network is down, and published for the dashboard
(aeroion/station/water and aeroion/station/water_event).

    python -m station.water --sim         try it without hardware: type taps and sensor values
    python -m station.water               on the Pi, with the probes, the valve, the flow meter and the reader
    python -m station.water report        what each card drew today

docs/water_point.md has the details.
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "contract"))  # makes `import aeroion_contract` work, as in tools/