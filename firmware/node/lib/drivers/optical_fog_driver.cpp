#include "optical_fog_driver.h"

#include "board_io.h"
#include "sensor_math.h"

namespace {
const int kSamples = 8;
const uint32_t kLedSettleUs = 300;
const float kSmoothing = 0.3f;  // weight of the newest reading
}  // namespace

// ---------------------------------------------------------------- optical sensor

OpticalFogDriver::OpticalFogDriver(uint8_t ledPin, uint8_t photoInput, float clearMv, float denseMv)
    : ledPin_(ledPin), photoInput_(photoInput), clearMv_(clearMv), denseMv_(denseMv), signalMv_(0), fog_(0),
      haveValue_(false) {}

bool OpticalFogDriver::begin() {
  io_pin_output(ledPin_, false);
  haveValue_ = false;
  return clearMv_ > denseMv_;  // a wrong calibration counts as missing
}

bool OpticalFogDriver::averageMv(float& mv) const {
  float sum = 0;
  for (int i = 0; i < kSamples; ++i) {
    float reading;
    if (!io_analog_mv(photoInput_, &reading)) return false;
    sum += reading;
  }
  mv = sum / kSamples;
  return true;
}

PollResult OpticalFogDriver::poll(uint32_t) {
  float ambientMv, litMv;
  if (!averageMv(ambientMv)) return PollResult::Error;
  io_pin_write(ledPin_, true);
  io_delay_us(kLedSettleUs);
  bool ok = averageMv(litMv);
  io_pin_write(ledPin_, false);
  if (!ok) return PollResult::Error;

  signalMv_ = litMv - ambientMv;
  if (signalMv_ < denseMv_ * 0.5f || signalMv_ > clearMv_ * 1.5f) return PollResult::Error;

  float index = sensor_math::fogIndex(signalMv_, clearMv_, denseMv_);
  fog_ = haveValue_ ? sensor_math::smooth(fog_, index, kSmoothing) : index;
  haveValue_ = true;
  return PollResult::Ok;
}

void OpticalFogDriver::fill(SensorFrame& frame) const { frame.fogIdxOpt = fog_; }

// ---------------------------------------------------------------- fog knob

FogKnobDriver::FogKnobDriver(uint8_t input, float fullScaleMv)
    : input_(input), fullScaleMv_(fullScaleMv), lastMv_(0), fog_(0) {}

PollResult FogKnobDriver::poll(uint32_t) {
  if (!io_analog_mv(input_, &lastMv_)) return PollResult::Error;
  fog_ = sensor_math::clamp(lastMv_ / fullScaleMv_, 0.0f, 1.0f);
  return PollResult::Ok;
}

void FogKnobDriver::fill(SensorFrame& frame) const { frame.fogIdxOpt = fog_; }