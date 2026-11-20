#pragma once
#include <Wire.h>

#include "sensor.h"

// INA219 current and voltage monitor on the HV module's 12 V input, I2C address 0x40.
// Gives the power drawn by the HV module (W) and the energy used since boot (Wh), which together with
// the collected mass gives grams per watt-hour.
class Ina219Sensor : public Sensor {
 public:
  Ina219Sensor(TwoWire& wire, float shuntOhms, uint8_t address = 0x40);
  const char* name() const override { return "INA219"; }
  bool begin() override;
  uint32_t periodMs() const override { return 200; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

  float busVolts() const { return busV_; }
  float currentMa() const { return currentA_ * 1000.0f; }

 private:
  bool writeRegister(uint8_t reg, uint16_t value);
  bool readRegister(uint8_t reg, uint16_t& value);

  TwoWire& wire_;
  float shuntOhms_;
  uint8_t address_;
  float busV_;
  float currentA_;
  float powerW_;
  float energyWh_;
  bool havePrevious_;
  uint32_t previousMs_;
};