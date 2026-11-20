#include "ina219_driver.h"

#include "board_io.h"
#include "sensor_math.h"

namespace {
const uint8_t kRegConfig = 0x00;
const uint8_t kRegShuntVoltage = 0x01;
const uint8_t kRegBusVoltage = 0x02;
// 32 V range, ±320 mV shunt range, 12-bit readings, shunt and bus measured continuously.
const uint16_t kConfig = 0x399F;
const uint32_t kMaxGapMs = 2000;  // ignore longer gaps instead of adding a burst of energy
}  // namespace

Ina219Driver::Ina219Driver(float shuntOhms, uint8_t address)
    : shuntOhms_(shuntOhms), address_(address), busV_(0), currentA_(0), powerW_(0), energyWh_(0),
      havePrevious_(false), previousMs_(0) {}

bool Ina219Driver::writeRegister(uint8_t reg, uint16_t value) {
  const uint8_t bytes[3] = {reg, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF)};
  return io_i2c_write(address_, bytes, sizeof bytes);
}

bool Ina219Driver::readRegister(uint8_t reg, uint16_t& value) {
  uint8_t bytes[2];
  if (!io_i2c_write_read(address_, &reg, 1, bytes, sizeof bytes)) return false;
  value = static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
  return true;
}

bool Ina219Driver::begin() {
  // Present if the configuration we write can be read back.
  uint16_t readBack = 0;
  return shuntOhms_ > 0.0f && writeRegister(kRegConfig, kConfig) && readRegister(kRegConfig, readBack) &&
         readBack == kConfig;
}

PollResult Ina219Driver::poll(uint32_t nowMs) {
  uint16_t shuntRaw, busRaw;
  if (!readRegister(kRegShuntVoltage, shuntRaw) || !readRegister(kRegBusVoltage, busRaw)) {
    return PollResult::Error;
  }
  if (sensor_math::ina219Overflow(busRaw)) return PollResult::Error;  // reading out of range

  busV_ = sensor_math::ina219BusVolts(busRaw);
  currentA_ = sensor_math::ina219ShuntMv(shuntRaw) / 1000.0f / shuntOhms_;
  powerW_ = busV_ * currentA_;
  if (powerW_ < 0.0f) powerW_ = 0.0f;  // tiny negative values are noise around zero current

  if (havePrevious_) {
    uint32_t gapMs = nowMs - previousMs_;
    if (gapMs <= kMaxGapMs) energyWh_ += powerW_ * gapMs / 3600000.0f;
  }
  havePrevious_ = true;
  previousMs_ = nowMs;
  return PollResult::Ok;
}

void Ina219Driver::fill(SensorFrame& frame) const {
  frame.powerW = powerW_;
  frame.energyWh = energyWh_;
}