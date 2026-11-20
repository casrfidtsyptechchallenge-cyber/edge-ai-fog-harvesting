#include "sht45_sensor.h"

#include <Arduino.h>

#include "sensor_math.h"

namespace {
const uint8_t kCmdMeasureHighPrecision = 0xFD;
const uint8_t kCmdReadSerial = 0x89;
const uint8_t kCmdSoftReset = 0x94;
const uint32_t kMeasureWaitMs = 10;  // datasheet: 8.3 ms max at high precision
}  // namespace

Sht45Sensor::Sht45Sensor(TwoWire& wire, uint8_t address)
    : wire_(wire), address_(address), tempC_(0), rhPct_(0) {}

bool Sht45Sensor::sendCommand(uint8_t command) {
  wire_.beginTransmission(address_);
  wire_.write(command);
  return wire_.endTransmission() == 0;
}

bool Sht45Sensor::readTwoWords(uint16_t& first, uint16_t& second) {
  uint8_t bytes[6];
  if (wire_.requestFrom(address_, static_cast<uint8_t>(6)) != 6) return false;
  for (int i = 0; i < 6; ++i) bytes[i] = static_cast<uint8_t>(wire_.read());
  if (sensor_math::sht4xCrc8(bytes, 2) != bytes[2] || sensor_math::sht4xCrc8(bytes + 3, 2) != bytes[5]) {
    return false;
  }
  first = static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
  second = static_cast<uint16_t>((bytes[3] << 8) | bytes[4]);
  return true;
}

bool Sht45Sensor::begin() {
  // The sensor is present if it answers with a serial number whose checksums are right.
  if (!sendCommand(kCmdSoftReset)) return false;
  delay(2);
  if (!sendCommand(kCmdReadSerial)) return false;
  delay(2);
  uint16_t high, low;
  return readTwoWords(high, low);
}

PollResult Sht45Sensor::poll(uint32_t) {
  if (!sendCommand(kCmdMeasureHighPrecision)) return PollResult::Error;
  delay(kMeasureWaitMs);
  uint16_t rawT, rawRh;
  if (!readTwoWords(rawT, rawRh)) return PollResult::Error;
  tempC_ = sensor_math::sht4xCelsius(rawT);
  rhPct_ = sensor_math::sht4xPercentRh(rawRh);
  return PollResult::Ok;
}

void Sht45Sensor::fill(SensorFrame& frame) const {
  frame.tempC = tempC_;
  frame.rhPct = rhPct_;
}