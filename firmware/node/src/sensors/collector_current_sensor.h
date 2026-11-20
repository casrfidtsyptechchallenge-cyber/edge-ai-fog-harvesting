#pragma once
#include "sensor.h"

// Corona current reaching the collector, measured as a voltage across a shunt resistor between the
// collector and ground, amplified by an op-amp. The collector side is near ground potential, so this
// is the safe place to measure.
//
//   current (µA) = (reading - zero) / amp gain / shunt ohms × 1000
//
// The zero is measured in begin(), at boot, when the node is LOCKED and the high voltage is off.
// M1 reports a smoothed average at 10 Hz. The fast windows (~10 kHz) for the arc predictor (M5a) will
// use the ESP32's continuous ADC mode behind this same class.
class CollectorCurrentSensor : public Sensor {
 public:
  CollectorCurrentSensor(uint8_t adcPin, float shuntOhms, float ampGain);
  const char* name() const override { return "COLLECTOR-I"; }
  bool begin() override;
  uint32_t periodMs() const override { return 100; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

  void zero();  // measure the zero again; call only while the high voltage is off
  float lastMv() const { return lastMv_; }
  float zeroMv() const { return zeroMv_; }

 private:
  float averageMv(int samples) const;

  uint8_t adcPin_;
  float shuntOhms_;
  float ampGain_;
  float zeroMv_;
  float lastMv_;
  float currentUa_;
  bool haveValue_;
};