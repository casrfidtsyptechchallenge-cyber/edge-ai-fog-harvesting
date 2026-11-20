#pragma once
#include "sensor.h"

// Pretends every sensor is installed and produces plausible, slowly changing values: a fog episode
// every 2 minutes, water collecting during fog, someone walking past the radar every 3 minutes.
// Lets the whole firmware run on a bare ESP32 or in a simulator before any hardware arrives.
// Turned on with AEROION_FAKE_SENSORS 1 in node_config.h.
class FakeSensor : public Sensor {
 public:
  FakeSensor();
  const char* name() const override { return "FAKE"; }
  bool begin() override;
  uint32_t periodMs() const override { return 200; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

 private:
  bool havePrevious_;
  uint32_t previousMs_;
  float fog_, tempC_, rhPct_, massG_, rateGpm_, currentUa_, powerW_, energyWh_;
  bool presence_;
};