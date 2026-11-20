#pragma once
#include <stdint.h>

// Unix time in milliseconds, for the message timestamps (ts) and to spot expired commands.
// The chip only counts milliseconds since boot; the link sends the real time every few seconds
// ({"link":"time","ts":...}). Before the first one, the clock counts from a fallback (the build time):
// the right range, not exact, and synced() is false, which the status message reports (time_synced).
class UnixClock {
 public:
  static const uint64_t kEarliestMs = 1704067200000ULL;  // 2024-01-01: an earlier time is not real
  static const uint64_t kLatestMs = 4102444800000ULL;    // 2100-01-01

  // fallbackMs: Unix time at boot until the first set(). staleAfterMs: synced() turns false when no time
  // has come for this long.
  explicit UnixClock(uint64_t fallbackMs, uint32_t staleAfterMs = 600000UL);

  // A time from the link. False (and nothing changes) if it is not a plausible time.
  bool set(uint64_t unixMs, uint32_t nowMs);

  // Unix time at nowMs (milliseconds since boot). nowMs may be a little older than the last one used.
  // Call at least once every 24 days: the 32-bit millisecond counter wraps after 49.7 days.
  uint64_t now(uint32_t nowMs);

  bool synced(uint32_t nowMs) const;  // set() within the last staleAfterMs
  bool everSet() const { return everSet_; }

 private:
  uint64_t monotonic(uint32_t nowMs);  // milliseconds since boot, without the 49-day wrap

  uint64_t offsetMs_;  // Unix time minus monotonic time
  uint32_t staleAfterMs_;
  bool everSet_;
  uint32_t lastSetMs_;
  bool started_;
  uint32_t lastNowMs_;   // the latest nowMs seen...
  uint64_t lastMonoMs_;  // ...and the same time, without the wrap
};