#include "lid_switch_sensor.h"

#include <Arduino.h>

LidSwitchSensor::LidSwitchSensor(uint8_t pin)
    : pin_(pin), closed_(false), candidate_(false), sameCount_(0), haveValue_(false) {}

bool LidSwitchSensor::begin() {
  pinMode(pin_, INPUT_PULLUP);
  sameCount_ = 0;
  haveValue_ = false;
  return true;
}

PollResult LidSwitchSensor::poll(uint32_t) {
  bool closedNow = digitalRead(pin_) == LOW;
  if (sameCount_ > 0 && closedNow == candidate_) {
    sameCount_++;
  } else {
    candidate_ = closedNow;
    sameCount_ = 1;
  }
  if (sameCount_ >= 2) {
    closed_ = candidate_;
    haveValue_ = true;
  }
  return haveValue_ ? PollResult::Ok : PollResult::NotReady;
}

void LidSwitchSensor::fill(SensorFrame& frame) const { frame.lidClosed = closed_; }