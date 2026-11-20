#include "switch_drivers.h"

#include "board_io.h"

// ---------------------------------------------------------------- radar

RadarPinDriver::RadarPinDriver(uint8_t pin) : pin_(pin), present_(true) {}

bool RadarPinDriver::begin() {
  io_pin_input_pullup(pin_);
  return true;
}

PollResult RadarPinDriver::poll(uint32_t) {
  present_ = io_pin_read(pin_);
  return PollResult::Ok;
}

void RadarPinDriver::fill(SensorFrame& frame) const { frame.radarPresence = present_; }

// ---------------------------------------------------------------- lid switch

LidPinDriver::LidPinDriver(uint8_t pin) : pin_(pin), closed_(false), candidate_(false), sameCount_(0), haveValue_(false) {}

bool LidPinDriver::begin() {
  io_pin_input_pullup(pin_);
  sameCount_ = 0;
  haveValue_ = false;
  return true;
}

PollResult LidPinDriver::poll(uint32_t) {
  bool closedNow = !io_pin_read(pin_);
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

void LidPinDriver::fill(SensorFrame& frame) const { frame.lidClosed = closed_; }