#pragma once
#include <stdint.h>

#include "aeroion_contract.h"

// Hardware watchdog: if loop() stops running for timeoutMs (a hang, an endless loop), the ESP32
// restarts. It then boots in LOCKED with the HV off, and main.cpp reports a FAULT.
void watchdogBegin(uint32_t timeoutMs);
void watchdogFeed();  // call once per loop()

// True if the last restart was abnormal (watchdog, crash, brown-out), with a fault code and a detail.
bool abnormalRestart(aeroion_fault_t* code, const char** detail);