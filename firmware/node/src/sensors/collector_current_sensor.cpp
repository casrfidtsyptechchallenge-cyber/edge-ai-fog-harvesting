#include "collector_current_sensor.h"

#include <Arduino.h>

#include "sensor_math.h"

namespace {
const int kSamples = 32;
const int kZeroSamples = 64;
const float kSmoothing = 0.3f;
}  // namespace

CollectorCurrentSensor::CollectorCurrentSensor(uint8_t adcPin, float shuntOhms, float ampGain)
    : adcPin_(adcPin), shuntOhms_(shuntOhms), ampGain_(ampGain), zeroMv_(0), lastMv_(0), currentUa_(0),
      haveValue_(false) {}

float CollectorCurrentSensor::averageMv(int samples) const {
  uint32_t sum = 0;
  for (int i = 0; i < samples; ++i) sum += analogReadMilliVolts(adcPin_);
  return static_cast<float>(sum) / samples;
}

void CollectorCurrentSensor::zero() {
  zeroMv_ = averageMv(kZeroSamples);
  haveValue_ = false;
}

bool CollectorCurrentSensor::begin() {
  if (shuntOhms_ <= 0.0f || ampGain_ <= 0.0f) return false;
  zero();
  return true;
}

PollResult CollectorCurrentSensor::poll(uint32_t) {
  lastMv_ = averageMv(kSamples);
  float ua = sensor_math::collectorMicroamps(lastMv_, zeroMv_, ampGain_, shuntOhms_);
  currentUa_ = haveValue_ ? sensor_math::smooth(currentUa_, ua, kSmoothing) : ua;
  haveValue_ = true;
  return PollResult::Ok;
}

void CollectorCurrentSensor::fill(SensorFrame& frame) const { frame.currentUa = currentUa_; }