#include "fake_sensor.h"

#include <math.h>

FakeSensor::FakeSensor()
    : havePrevious_(false), previousMs_(0), fog_(0), tempC_(0), rhPct_(0), massG_(0), rateGpm_(0),
      currentUa_(0), powerW_(0), energyWh_(0), presence_(false) {}

bool FakeSensor::begin() {
  havePrevious_ = false;
  massG_ = 0;
  energyWh_ = 0;
  return true;
}

PollResult FakeSensor::poll(uint32_t nowMs) {
  float dtS = havePrevious_ ? (nowMs - previousMs_) / 1000.0f : 0.0f;
  havePrevious_ = true;
  previousMs_ = nowMs;

  float t = nowMs / 1000.0f;
  float wave = sinf(2.0f * 3.14159265f * t / 120.0f);  // one fog episode every 120 s
  fog_ = wave > 0 ? 0.85f * wave : 0.0f;
  tempC_ = 14.0f - 1.5f * fog_ + 0.2f * sinf(t / 7.0f);
  rhPct_ = fminf(100.0f, 72.0f + 28.0f * fog_);
  rateGpm_ = 0.9f * fog_;
  massG_ += rateGpm_ * dtS / 60.0f;
  currentUa_ = fog_ > 0.3f ? 25.0f * (1.0f - 0.45f * fog_) : 0.2f;
  powerW_ = fog_ > 0.3f ? 1.0f : 0.0f;
  energyWh_ += powerW_ * dtS / 3600.0f;
  presence_ = fmodf(t, 180.0f) > 170.0f;  // someone walks past for 10 s every 3 minutes
  return PollResult::Ok;
}

void FakeSensor::fill(SensorFrame& frame) const {
  frame.fogIdxOpt = fog_;
  frame.tempC = tempC_;
  frame.rhPct = rhPct_;
  frame.massG = massG_;
  frame.rateGpm = rateGpm_;
  frame.currentUa = currentUa_;
  frame.powerW = powerW_;
  frame.energyWh = energyWh_;
  frame.radarPresence = presence_;
  frame.lidClosed = true;
  // battPct stays null: the fake node runs on "bench power".
}