#include "fog_knob_sensor.h"

#include <Arduino.h>

#include "sensor_math.h"

FogKnobSensor::FogKnobSensor(uint8_t adcPin) : adcPin_(adcPin), fog_(0.0f) {}

PollResult FogKnobSensor::poll(uint32_t) {
  fog_ = sensor_math::clamp(analogReadMilliVolts(adcPin_) / 3300.0f, 0.0f, 1.0f);
  return PollResult::Ok;
}

void FogKnobSensor::fill(SensorFrame& frame) const { frame.fogIdxOpt = fog_; }