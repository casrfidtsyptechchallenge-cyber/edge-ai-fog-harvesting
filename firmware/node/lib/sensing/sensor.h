#pragma once
#include <stdint.h>

#include "sensor_frame.h"

enum class PollResult : uint8_t {
  Ok,        // a new valid reading is stored
  NotReady,  // nothing new yet, and that is normal (e.g. a conversion is still running)
  Error,     // the sensor answered wrongly or not at all
};

// What every sensor driver implements. The registry calls these methods;
// no other part of the firmware needs to know which sensors exist.
class Sensor {
 public:
  virtual ~Sensor() {}

  // Short id such as "SHT45". Listed in the info message.
  virtual const char* name() const = 0;

  // Set up the hardware. Return false if the sensor does not answer; it will be tried again later.
  virtual bool begin() = 0;

  // How often poll() should run.
  virtual uint32_t periodMs() const = 0;

  // Read the hardware and keep the latest value inside the driver.
  virtual PollResult poll(uint32_t nowMs) = 0;

  // Copy the latest value into the frame. Write only this sensor's own fields.
  virtual void fill(SensorFrame& frame) const = 0;

  // A reading older than this is treated as missing (null).
  virtual uint32_t staleAfterMs() const { return 3 * periodMs() + 1000; }
};