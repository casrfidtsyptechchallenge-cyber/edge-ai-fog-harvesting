#pragma once
#include "sensor.h"

// Fog density from how much light the fog blocks: an IR LED on one side of the box, a photodiode on
// the other. Each reading measures the photodiode with the LED off (ambient light) and on, and keeps
// the difference, so room light does not matter.
//
//   fog index = (clear - signal) / (clear - dense), clamped to 0..1
//
// Calibration: set AEROION_DEBUG_RAW 1 and note the signal in clear air (FOG_CLEAR_MV) and in your
// densest fog (FOG_DENSE_MV). A signal far outside that range (sensor unplugged, blocked, or flooded
// with light) is reported as an error, so the fog index becomes null instead of a false value.
class OpticalFogSensor : public Sensor {
 public:
  OpticalFogSensor(uint8_t ledPin, uint8_t photoPin, float clearMv, float denseMv);
  const char* name() const override { return "OPTICAL-FOG"; }
  bool begin() override;
  uint32_t periodMs() const override { return 200; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

  float lastSignalMv() const { return signalMv_; }

 private:
  float averageMv(int samples) const;

  uint8_t ledPin_;
  uint8_t photoPin_;
  float clearMv_;
  float denseMv_;
  float signalMv_;
  float fog_;
  bool haveValue_;
};