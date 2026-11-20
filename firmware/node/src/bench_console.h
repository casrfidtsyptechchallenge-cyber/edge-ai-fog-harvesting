#pragma once
#include <stddef.h>
#include <stdint.h>

#include "safety_core.h"

// Commands typed in the serial monitor, for bench tests and the simulator, until the RFID reader (M4)
// and MQTT commands (M7) exist. Type "help". Answers are '#' lines; events still go out as messages.
// Field nodes: set AEROION_BENCH_CONSOLE 0 in node_config.h, or anyone with a USB cable could arm.
class BenchConsole {
 public:
  explicit BenchConsole(SafetyCore& safety);
  void poll(uint32_t nowMs);  // reads what has arrived; runs each complete line

 private:
  void run(char* line, uint32_t nowMs);
  void tap(const char* uid, aeroion_role_t role);
  void printState();

  SafetyCore& safety_;
  char line_[96];
  size_t length_;
};