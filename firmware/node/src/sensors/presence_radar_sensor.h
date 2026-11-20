#pragma once
#include "sensor.h"

// Person-presence radar (LD2410) read through its OUT pin: HIGH while someone is in range.
// Fail-safe wiring: the pin has a pull-up, so a broken wire reads HIGH = "person present", which keeps
// the high voltage off (M3 decides that; this class only reports).
class PresenceRadarSensor : public Sensor {
 public:
  explicit PresenceRadarSensor(uint8_t outPin);
  const char* name() const override { return "LD2410"; }
  bool begin() override;
  uint32_t periodMs() const override { return 100; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

 private:
  uint8_t outPin_;
  bool present_;
};