"""The water quality gate (M9, F7): TDS and pH decide whether the valve may open.

The probes are read about once a second. The gate looks at the median of the last 3 readings, so one wrong
reading neither closes nor opens the valve, and:

- the verdict is bad as soon as that median is out of limits (two bad readings in a row, about 2 s);
- it becomes ok only after the readings have been good for settle_s in a row (a probe just put back in the
  water reads wrong for a few seconds);
- it is unknown when a probe gives nothing, or nothing newer than max_age_s: the valve stays closed, as
  for bad.

Plain logic on given times, tested on its own (tests/test_water_quality.py).
"""

import math
import statistics
from collections import deque
from dataclasses import dataclass


@dataclass
class Limits:
    tds_max_ppm: float = 500.0   # the usual drinking-water guide value (WHO: under about 600 mg/L tastes good)
    ph_min: float = 6.5          # WHO and most national standards: 6.5 to 8.5
    ph_max: float = 8.5
    max_age_s: float = 5.0       # an older reading is no reading
    settle_s: float = 3.0        # good readings needed in a row before the verdict turns ok
    window: int = 3              # median of the last 3 readings


@dataclass
class Reading:
    ts_ms: int
    tds_ppm: float = None        # None: no value (probe missing, out of range)
    ph: float = None
    temp_c: float = None


@dataclass(frozen=True)
class Verdict:
    verdict: str                 # "ok", "bad" or "unknown" (water.schema.json)
    reason: str
    tds_ppm: float = None
    ph: float = None
    temp_c: float = None

    def as_dict(self):
        return {"verdict": self.verdict, "reason": self.reason, "tds_ppm": self.tds_ppm, "ph": self.ph,
                "temp_c": self.temp_c}


def _within(value, low, high):
    """A real number in [low, high], or None: a probe's nonsense is no reading."""
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        return None
    return float(value) if low <= value <= high else None


def _median(values, digits):
    values = [v for v in values if v is not None]
    return round(statistics.median(values), digits) if values else None


class QualityGate:
    def __init__(self, limits=None):
        self.limits = limits or Limits()
        self.readings = deque(maxlen=self.limits.window)
        self.good_since = None
        self.current = Verdict("unknown", "starting: no reading yet")

    def add(self, reading):
        self.readings.append(Reading(int(reading.ts_ms), _within(reading.tds_ppm, 0, 5000),
                                     _within(reading.ph, 0, 14), _within(reading.temp_c, -10, 80)))

    def evaluate(self, now_ms):
        """The verdict now. Call it after each reading and in between (old readings expire)."""
        L = self.limits
        fresh = [r for r in self.readings if now_ms - r.ts_ms <= L.max_age_s * 1000]
        tds = _median([r.tds_ppm for r in fresh], 0)
        ph = _median([r.ph for r in fresh], 2)
        temp = _median([r.temp_c for r in fresh], 1)
        if tds is None and ph is None:
            verdict, reason = "unknown", "no reading from the probes"
        elif tds is None:
            verdict, reason = "unknown", "no TDS reading"
        elif ph is None:
            verdict, reason = "unknown", "no pH reading"
        elif tds > L.tds_max_ppm:
            verdict, reason = "bad", f"TDS {tds:.0f} ppm, above {L.tds_max_ppm:g}"
        elif ph < L.ph_min:
            verdict, reason = "bad", f"pH {ph:.1f}, below {L.ph_min:g}"
        elif ph > L.ph_max:
            verdict, reason = "bad", f"pH {ph:.1f}, above {L.ph_max:g}"
        else:
            verdict, reason = "ok", ""
        if verdict == "ok":
            if self.good_since is None:
                self.good_since = now_ms
            good_s = (now_ms - self.good_since) / 1000
            if self.current.verdict != "ok" and good_s < L.settle_s:
                verdict = self.current.verdict  # not yet: it keeps its last verdict while it checks
                reason = f"readings good, checking ({good_s:.0f} of {L.settle_s:g} s)"
        else:
            self.good_since = None
        self.current = Verdict(verdict, reason, tds, ph, temp)
        return self.current