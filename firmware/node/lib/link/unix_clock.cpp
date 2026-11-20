#include "unix_clock.h"

UnixClock::UnixClock(uint64_t fallbackMs, uint32_t staleAfterMs)
    : offsetMs_(fallbackMs),
      staleAfterMs_(staleAfterMs),
      everSet_(false),
      lastSetMs_(0),
      started_(false),
      lastNowMs_(0),
      lastMonoMs_(0) {}

uint64_t UnixClock::monotonic(uint32_t nowMs) {
  if (!started_) {
    started_ = true;
    lastNowMs_ = nowMs;
    lastMonoMs_ = nowMs;
  }
  // The difference of two 32-bit counts is right across the wrap; negative for a time read a little earlier.
  int32_t delta = static_cast<int32_t>(nowMs - lastNowMs_);
  uint64_t mono = lastMonoMs_ + static_cast<int64_t>(delta);
  if (delta > 0) {
    lastNowMs_ = nowMs;
    lastMonoMs_ = mono;
  }
  return mono;
}

bool UnixClock::set(uint64_t unixMs, uint32_t nowMs) {
  if (unixMs < kEarliestMs || unixMs > kLatestMs) return false;
  offsetMs_ = unixMs - monotonic(nowMs);
  everSet_ = true;
  lastSetMs_ = nowMs;
  return true;
}

uint64_t UnixClock::now(uint32_t nowMs) { return offsetMs_ + monotonic(nowMs); }

bool UnixClock::synced(uint32_t nowMs) const { return everSet_ && nowMs - lastSetMs_ < staleAfterMs_; }