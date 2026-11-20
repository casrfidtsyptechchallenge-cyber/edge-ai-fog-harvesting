#include "collector_current_driver.h"

#include "board_io.h"
#include "sensor_math.h"

namespace {
const int kSamples = 32;
const int kZeroSamples = 64;
}  // namespace

CollectorCurrentDriver::CollectorCurrentDriver(uint8_t input, float shuntOhms, float ampGain)
    : input_(input), shuntOhms_(shuntOhms), ampGain_(ampGain), zeroed_(false), zeroMv_(0), lastMv_(0),
      currentUa_(0) {}

bool CollectorCurrentDriver::averageMv(int samples, float& mv) const {
  float sum = 0;
  for (int i = 0; i < samples; ++i) {
    float reading;
    if (!io_analog_mv(input_, &reading)) return false;
    sum += reading;
  }
  mv = sum / samples;
  return true;
}

bool CollectorCurrentDriver::zero() {
  float mv;
  if (!averageMv(kZeroSamples, mv)) return false;
  zeroMv_ = mv;
  zeroed_ = true;
  return true;
}

bool CollectorCurrentDriver::begin() {
  if (shuntOhms_ <= 0.0f || ampGain_ <= 0.0f) return false;  // a wrong calibration counts as missing
  return zeroed_ || zero();
}

PollResult CollectorCurrentDriver::poll(uint32_t) {
  if (!averageMv(kSamples, lastMv_)) return PollResult::Error;
  currentUa_ = sensor_math::collectorMicroamps(lastMv_, zeroMv_, ampGain_, shuntOhms_);
  return PollResult::Ok;
}

void CollectorCurrentDriver::fill(SensorFrame& frame) const { frame.currentUa = currentUa_; }