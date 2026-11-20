#pragma once
#include <stdint.h>

// Unix time in milliseconds.
// M1: estimated from the build time plus the time since boot, so timestamps are in the right range
// but not exact (time_synced = false). M7 will replace this with time from the station (NTP)
// without changing any caller.
uint64_t nodeTimeMs();
bool nodeTimeSynced();