#pragma once
#include "rate_estimator.h"
#include "sensor.h"

// HX711 load-cell amplifier under the collector tray: gives the water mass (g) and, from its slope over
// the last 30 s, the collection rate (g/min). The first 10 readings after start-up or tare() set the zero.
//
// Calibration: set AEROION_DEBUG_RAW 1, note the raw counts empty and with a known weight, then
// HX711_COUNTS_PER_GRAM = (raw with weight - raw empty) / weight in grams. It may be negative.
class Hx711Sensor : public Sensor {
 public:
  Hx711Sensor(uint8_t doutPin, uint8_t sckPin, float countsPerGram);
  const char* name() const override { return "HX711"; }
  bool begin() override;
  uint32_t periodMs() const override { return 100; }  // the HX711 makes 10 readings per second
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

  void tare();  // the next readings become the new zero (used by the TARE command in M7)
  long lastRaw() const { return raw_; }

 private:
  static const int kTareSamples = 10;
  bool isReady() const;
  long readRaw();

  uint8_t dout_;
  uint8_t sck_;
  float countsPerGram_;
  long raw_;
  long long tareSum_;
  int tareLeft_;
  long offset_;
  float massG_;
  uint32_t lastRateSampleMs_;
  bool haveRateSample_;
  RateEstimator<30> rate_;  // one sample per second over 30 s
};