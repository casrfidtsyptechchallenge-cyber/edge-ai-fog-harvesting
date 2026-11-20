#include "optical_fog_sensor.h"

#include <Arduino.h>

#include "sensor_math.h"

namespace {
const int kSamples = 8;
const uint32_t kLedSettleUs = 300;
const float kSmoothing = 0.3f;  // weight of the newest reading
}  // namespace

OpticalFogSensor::OpticalFogSensor(uint8_t ledPin, uint8_t photoPin, float clearMv, float denseMv)
    : ledPin_(ledPin), photoPin_(photoPin), clearMv_(clearMv), denseMv_(denseMv), signalMv_(0), fog_(0),
      haveValue_(false) {}

bool OpticalFogSensor::begin() {
  pinMode(ledPin_, OUTPUT);
  digitalWrite(ledPin_, LOW);
  haveValue_ = false;
  return clearMv_ > denseMv_;  // a wrong calibration is treated like a missing sensor
}

float OpticalFogSensor::averageMv(int samples) const {
  uint32_t sum = 0;
  for (int i = 0; i < samples; ++i) sum += analogReadMilliVolts(photoPin_);
  return static_cast<float>(sum) / samples;
}

PollResult OpticalFogSensor::poll(uint32_t) {
  float ambientMv = averageMv(kSamples);
  digitalWrite(ledPin_, HIGH);
  delayMicroseconds(kLedSettleUs);
  float litMv = averageMv(kSamples);
  digitalWrite(ledPin_, LOW);
  signalMv_ = litMv - ambientMv;

  if (signalMv_ < denseMv_ * 0.5f || signalMv_ > clearMv_ * 1.5f) return PollResult::Error;

  float index = sensor_math::fogIndex(signalMv_, clearMv_, denseMv_);
  fog_ = haveValue_ ? sensor_math::smooth(fog_, index, kSmoothing) : index;
  haveValue_ = true;
  return PollResult::Ok;
}

void OpticalFogSensor::fill(SensorFrame& frame) const { frame.fogIdxOpt = fog_; }