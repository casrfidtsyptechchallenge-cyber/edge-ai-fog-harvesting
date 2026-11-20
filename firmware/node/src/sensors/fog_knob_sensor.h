#pragma once
#include "sensor.h"

// Simulator stand-in for the optical fog sensor: a potentiometer sets the fog index directly
// (0 V = clear air, 3.3 V = dense fog), so a Wokwi demo can drive the safety core by hand.
// Do not install it together with AEROION_HAS_OPTICAL_FOG: both fill fog_idx_opt.
class FogKnobSensor : public Sensor {
 public:
  explicit FogKnobSensor(uint8_t adcPin);
  const char* name() const override { return "FOG-KNOB"; }
  bool begin() override { return true; }
  uint32_t periodMs() const override { return 200; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

 private:
  uint8_t adcPin_;
  float fog_;
};