#pragma once
#include <Wire.h>

#include "sensor.h"

// Sensirion SHT45 temperature and humidity sensor, I2C address 0x44.
// Datasheet: send command 0xFD (high precision), wait 10 ms, read 6 bytes:
// T msb, T lsb, CRC, RH msb, RH lsb, CRC.
// Note for fog: liquid water on the sensor makes RH read 100 % for a while. Mount it at the box inlet,
// or use its built-in heater later to dry it.
class Sht45Sensor : public Sensor {
 public:
  explicit Sht45Sensor(TwoWire& wire, uint8_t address = 0x44);
  const char* name() const override { return "SHT45"; }
  bool begin() override;
  uint32_t periodMs() const override { return 1000; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame& frame) const override;

 private:
  bool sendCommand(uint8_t command);
  bool readTwoWords(uint16_t& first, uint16_t& second);

  TwoWire& wire_;
  uint8_t address_;
  float tempC_;
  float rhPct_;
};