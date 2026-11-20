#pragma once
#include "sensor.h"

// Fog density from how much light the fog blocks: an IR LED on one side of the box, a photodiode on the
// other. Each reading measures the photodiode with the LED off (ambient light) and on, and keeps the
// difference, so room light does not matter.
//
//   fog index = (clear - signal) / (clear - dense), clamped to 0..1      (sensor_math.h)
//
// A signal far outside the calibrated range (sensor unplugged, blocked, or flooded with light) is an error,
// so the fog index becomes null instead of a false value.
class OpticalFogDriver : public Sensor {
 public:
  OpticalFogDriver(uint8_t ledPin, uint8_t photoInput, float clearMv, float denseMv);
  const char* name() const override { return "OPTICAL-FOG"; }
  bool begin() override;
  uint32_t periodMs() const override { return 200; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

  float lastSignalMv() const { return signalMv_; }

 private:
  bool averageMv(float& mv) const;

  uint8_t ledPin_;
  uint8_t photoInput_;
  float clearMv_;
  float denseMv_;
  float signalMv_;
  float fog_;
  bool haveValue_;
};

// Bench stand-in for the optical sensor: a potentiometer sets the fog index directly
// (0 V = clear air, full scale = dense fog), so the safety core can be driven by hand.
// Do not install it together with the optical sensor: both fill fog_idx_opt.
class FogKnobDriver : public Sensor {
 public:
  FogKnobDriver(uint8_t input, float fullScaleMv);
  const char* name() const override { return "FOG-KNOB"; }
  bool begin() override { return fullScaleMv_ > 0.0f; }
  uint32_t periodMs() const override { return 200; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

  float lastMv() const { return lastMv_; }

 private:
  uint8_t input_;
  float fullScaleMv_;
  float lastMv_;
  float fog_;
};