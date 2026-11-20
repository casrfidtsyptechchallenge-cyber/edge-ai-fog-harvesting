#include "hv_output.h"

#include <Arduino.h>

namespace {
const uint32_t kPwmHz = 20000;  // above hearing, easy to filter
const uint8_t kPwmBits = 10;    // duty 0..1023
const uint8_t kPwmChannel = 0;  // only used by Arduino-ESP32 2.x
const uint32_t kPwmMax = (1u << kPwmBits) - 1;
}  // namespace

HvOutput::HvOutput(uint8_t enablePin, uint8_t levelPin) : enablePin_(enablePin), levelPin_(levelPin), on_(false) {}

void HvOutput::begin() {
  digitalWrite(enablePin_, LOW);  // set the level before the pin becomes an output: no glitch
  pinMode(enablePin_, OUTPUT);
  digitalWrite(enablePin_, LOW);
  // The PWM functions changed in Arduino-ESP32 3.x; this works with both.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(levelPin_, kPwmHz, kPwmBits);
#else
  ledcSetup(kPwmChannel, kPwmHz, kPwmBits);
  ledcAttachPin(levelPin_, kPwmChannel);
#endif
  writeLevel(0.0f);
  on_ = false;
}

void HvOutput::apply(bool on, float level) {
  if (!on) {
    digitalWrite(enablePin_, LOW);  // supply off first, then the control voltage
    writeLevel(0.0f);
  } else {
    writeLevel(level);
    digitalWrite(enablePin_, HIGH);
  }
  on_ = on;
}

void HvOutput::writeLevel(float level) {
  if (!(level > 0.0f)) level = 0.0f;  // also catches NaN
  if (level > 1.0f) level = 1.0f;
  uint32_t duty = static_cast<uint32_t>(level * kPwmMax + 0.5f);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(levelPin_, duty);
#else
  ledcWrite(kPwmChannel, duty);
#endif
}