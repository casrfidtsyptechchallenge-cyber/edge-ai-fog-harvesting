#pragma once
#include <stdint.h>

#include "sensor.h"

enum class SensorState : uint8_t {
  Missing,  // begin() failed; looked for again every kRetryMs
  Waiting,  // started, no valid reading yet
  Ok,       // recent valid reading
  Stale,    // had readings, but none recently: its fields are null
};

const char* sensorStateName(SensorState state);

// Holds the installed sensors, polls each one on its own schedule and assembles the frame.
// Fixed size, no dynamic memory.
class SensorRegistry {
 public:
  static const int kMaxSensors = 12;
  static const uint32_t kRetryMs = 5000;           // how often a missing sensor is looked for again
  static const uint32_t kMaxConsecutiveErrors = 10;  // then the sensor is set up again from scratch

  SensorRegistry();

  bool add(Sensor* sensor);  // false if full or null
  void beginAll(uint32_t nowMs);
  void pollAll(uint32_t nowMs);
  void fillFrame(SensorFrame& frame, uint32_t nowMs) const;  // clears the frame, then fills it

  int count() const { return count_; }
  const Sensor* sensorAt(int index) const;
  SensorState stateAt(int index, uint32_t nowMs) const;
  uint32_t errorsAt(int index) const;

 private:
  struct Slot {
    Sensor* sensor;
    bool started;
    bool everOk;
    uint32_t lastPollMs;
    uint32_t lastOkMs;
    uint32_t errors;
    uint32_t consecutiveErrors;
  };

  void tryBegin(Slot& slot, uint32_t nowMs);
  bool isFresh(const Slot& slot, uint32_t nowMs) const;

  Slot slots_[kMaxSensors];
  int count_;
};