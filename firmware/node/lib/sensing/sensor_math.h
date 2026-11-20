#pragma once
#include <stdint.h>

// The pure formulas used by the sensor drivers: no hardware access, so they are tested on the PC
// (test/test_sensor_math). The drivers in src/sensors/ only move bytes and call these.
namespace sensor_math {

inline float clamp(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}

// Exponential moving average: `weight` is how much the newest reading counts (0..1).
inline float smooth(float previous, float latest, float weight) { return previous + weight * (latest - previous); }

// ---- SHT45 (Sensirion datasheet) ----

// CRC-8, polynomial 0x31, initial value 0xFF.
inline uint8_t sht4xCrc8(const uint8_t* data, int length) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

inline float sht4xCelsius(uint16_t raw) { return -45.0f + 175.0f * raw / 65535.0f; }

inline float sht4xPercentRh(uint16_t raw) { return clamp(-6.0f + 125.0f * raw / 65535.0f, 0.0f, 100.0f); }

// ---- HX711 ----

// The HX711 sends 24-bit two's complement numbers.
inline long hx711Signed(unsigned long raw24) {
  raw24 &= 0xFFFFFFUL;
  return (raw24 & 0x800000UL) ? static_cast<long>(raw24) - 0x1000000L : static_cast<long>(raw24);
}

// ---- INA219 (TI datasheet) ----

inline float ina219ShuntMv(uint16_t shuntRegister) { return static_cast<int16_t>(shuntRegister) * 0.01f; }  // 10 µV/bit

inline float ina219BusVolts(uint16_t busRegister) { return (busRegister >> 3) * 0.004f; }  // 4 mV/bit, top 13 bits

inline bool ina219Overflow(uint16_t busRegister) { return (busRegister & 0x0001) != 0; }

// ---- Optical fog sensor ----

// 0 = clear air, 1 = densest calibrated fog.
inline float fogIndex(float signalMv, float clearMv, float denseMv) {
  return clamp((clearMv - signalMv) / (clearMv - denseMv), 0.0f, 1.0f);
}

// ---- Collector current ----

// Shunt voltage, amplified by the op-amp, converted to µA. Never negative.
inline float collectorMicroamps(float readingMv, float zeroMv, float ampGain, float shuntOhms) {
  float microamps = (readingMv - zeroMv) / ampGain / shuntOhms * 1000.0f;
  return microamps < 0.0f ? 0.0f : microamps;
}

}  // namespace sensor_math