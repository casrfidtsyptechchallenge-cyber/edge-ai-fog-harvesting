#include "node_clock.h"

#include <esp_timer.h>

#ifndef AEROION_BUILD_EPOCH_S
#define AEROION_BUILD_EPOCH_S 1767225600ULL  // 2026-01-01, used only if the build did not set it
#endif

uint64_t nodeTimeMs() {
  // esp_timer_get_time() counts microseconds since boot in 64 bits, so it never wraps (millis() does).
  return static_cast<uint64_t>(AEROION_BUILD_EPOCH_S) * 1000ULL +
         static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

bool nodeTimeSynced() { return false; }