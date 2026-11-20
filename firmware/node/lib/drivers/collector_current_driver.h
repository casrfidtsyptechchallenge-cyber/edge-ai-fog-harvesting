#pragma once
#include "sensor.h"

// Corona current reaching the collector: the voltage across a shunt resistor between the collector and
// ground, amplified by an op-amp, on an analog input. The collector side is near ground potential, so this
// is the safe place to measure.
//
//   current (µA) = (reading - zero) / amp gain / shunt ohms × 1000      (sensor_math.h)
//
// Each reading is the average of 32 conversions (well under a millisecond) and is not smoothed any further:
// the safety core (M3) must see a current spike in the very next reading, not a few readings later.
//
// The zero is measured once, by the first begin() at boot, while the node is LOCKED and the HV is off.
// If the driver is set up again later (after many read errors), it keeps that zero: measuring it with the
// HV on would hide the corona current, and with it an overcurrent. zero() measures it again on request.
class CollectorCurrentDriver : public Sensor {
 public:
  CollectorCurrentDriver(uint8_t input, float shuntOhms, float ampGain);
  const char* name() const override { return "COLLECTOR-I"; }
  bool begin() override;
  uint32_t periodMs() const override { return 100; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

  bool zero();  // measure the zero again; call only while the high voltage is off
  float lastMv() const { return lastMv_; }
  float zeroMv() const { return zeroMv_; }

 private:
  bool averageMv(int samples, float& mv) const;

  uint8_t input_;
  float shuntOhms_;
  float ampGain_;
  bool zeroed_;
  float zeroMv_;
  float lastMv_;
  float currentUa_;
};