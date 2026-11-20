#pragma once
#include "sensor.h"

// Enclosure lid switch between the pin and ground: closed lid = switch closed = LOW.
// Fail-safe wiring: the pin has a pull-up, so a broken wire reads HIGH = "lid open".
// A change is accepted only after two equal readings in a row (debounce).
class LidSwitchSensor : public Sensor {
 public:
  explicit LidSwitchSensor(uint8_t pin);
  const char* name() const override { return "LID"; }
  bool begin() override;
  uint32_t periodMs() const override { return 50; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

 private:
  uint8_t pin_;
  bool closed_;
  bool candidate_;
  int sameCount_;
  bool haveValue_;
};