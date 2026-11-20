import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO)) # for `import station.datalog ` <-- New
for sub in ("contract", "sim", "tools"):
    sys.path.insert(0, str(REPO / sub))
