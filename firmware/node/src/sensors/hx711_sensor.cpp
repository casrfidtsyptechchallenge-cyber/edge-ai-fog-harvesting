#include "hx711_sensor.h"

#include <Arduino.h>

#include "sensor_math.h"

namespace {
// The HX711 powers down if its clock stays high for more than 60 µs, so the 25 clock pulses of one
// reading must not be interrupted. A critical section blocks interrupts for those ~50 µs.
portMUX_TYPE hx711Mux = portMUX_INITIALIZER_UNLOCKED;
const long kSaturatedHigh = 0x7FFFFF;
const long kSaturatedLow = -0x800000;
}  // namespace

Hx711Sensor::Hx711Sensor(uint8_t doutPin, uint8_t sckPin, float countsPerGram)
    : dout_(doutPin), sck_(sckPin), countsPerGram_(countsPerGram), raw_(0), tareSum_(0),
      tareLeft_(kTareSamples), offset_(0), massG_(0), lastRateSampleMs_(0), haveRateSample_(false) {}

bool Hx711Sensor::isReady() const { return digitalRead(dout_) == LOW; }

bool Hx711Sensor::begin() {
  // With a pull-up, a missing HX711 reads HIGH forever. A present one pulls DOUT low within 100 ms.
  pinMode(dout_, INPUT_PULLUP);
  pinMode(sck_, OUTPUT);
  digitalWrite(sck_, LOW);
  uint32_t start = millis();
  while (millis() - start < 200) {
    if (isReady()) {
      tare();
      return true;
    }
    delay(5);
  }
  return false;
}

void Hx711Sensor::tare() {
  tareSum_ = 0;
  tareLeft_ = kTareSamples;
  rate_.reset();
  haveRateSample_ = false;
}

long Hx711Sensor::readRaw() {
  unsigned long value = 0;
  portENTER_CRITICAL(&hx711Mux);
  for (int i = 0; i < 24; ++i) {
    digitalWrite(sck_, HIGH);
    delayMicroseconds(1);
    value = (value << 1) | (digitalRead(dout_) == HIGH ? 1UL : 0UL);
    digitalWrite(sck_, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(sck_, HIGH);  // 25th pulse: next reading from channel A, gain 128
  delayMicroseconds(1);
  digitalWrite(sck_, LOW);
  delayMicroseconds(1);
  portEXIT_CRITICAL(&hx711Mux);
  return sensor_math::hx711Signed(value);
}

PollResult Hx711Sensor::poll(uint32_t nowMs) {
  if (!isReady()) return PollResult::NotReady;
  raw_ = readRaw();
  if (raw_ == kSaturatedHigh || raw_ == kSaturatedLow || countsPerGram_ == 0.0f) return PollResult::Error;

  if (tareLeft_ > 0) {  // still collecting the zero
    tareSum_ += raw_;
    if (--tareLeft_ == 0) offset_ = static_cast<long>(tareSum_ / kTareSamples);
    return PollResult::NotReady;
  }

  massG_ = (raw_ - offset_) / countsPerGram_;
  if (!haveRateSample_ || nowMs - lastRateSampleMs_ >= 1000) {
    rate_.add(nowMs, massG_);
    lastRateSampleMs_ = nowMs;
    haveRateSample_ = true;
  }
  return PollResult::Ok;
}

void Hx711Sensor::fill(SensorFrame& frame) const {
  frame.massG = massG_;
  if (rate_.ready()) frame.rateGpm = rate_.perMinute();
}