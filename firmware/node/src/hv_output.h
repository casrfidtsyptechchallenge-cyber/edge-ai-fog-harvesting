#pragma once
#include <stdint.h>

// The two signals that drive the high-voltage module. The ESP32 never touches the high voltage itself:
//   enable pin -> MOSFET (or relay) that switches the module's 12 V supply
//   level pin  -> PWM, smoothed by an RC filter into the module's 0..3.3 V control input
// Fail-safe wiring: a 10 kOhm pull-down on the MOSFET gate keeps the HV off while the ESP32 boots,
// resets or hangs. The lid switch should also cut the 12 V in hardware, so safety never depends on
// software alone.
class HvOutput {
 public:
  HvOutput(uint8_t enablePin, uint8_t levelPin);
  void begin();                      // HV off. Call it first in setup(), before anything else.
  void apply(bool on, float level);  // level 0..1; ignored (0) when off
  bool isOn() const { return on_; }

 private:
  void writeLevel(float level);

  uint8_t enablePin_;
  uint8_t levelPin_;
  bool on_;
};