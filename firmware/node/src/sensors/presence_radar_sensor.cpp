#include "presence_radar_sensor.h"

#include <Arduino.h>

PresenceRadarSensor::PresenceRadarSensor(uint8_t outPin) : outPin_(outPin), present_(true) {}

bool PresenceRadarSensor::begin() {
  pinMode(outPin_, INPUT_PULLUP);
  return true;
}

PollResult PresenceRadarSensor::poll(uint32_t) {
  present_ = digitalRead(outPin_) == HIGH;
  return PollResult::Ok;
}

void PresenceRadarSensor::fill(SensorFrame& frame) const { frame.radarPresence = present_; }