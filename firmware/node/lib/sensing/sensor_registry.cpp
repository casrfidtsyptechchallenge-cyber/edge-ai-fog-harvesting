#include "sensor_registry.h"

const char* sensorStateName(SensorState state) {
  switch (state) {
    case SensorState::Missing: return "missing";
    case SensorState::Waiting: return "waiting";
    case SensorState::Ok: return "ok";
    case SensorState::Stale: return "stale";
  }
  return "?";
}

SensorRegistry::SensorRegistry() : count_(0) {}

bool SensorRegistry::add(Sensor* sensor) {
  if (sensor == nullptr || count_ >= kMaxSensors) return false;
  Slot& slot = slots_[count_++];
  slot.sensor = sensor;
  slot.started = false;
  slot.everOk = false;
  slot.lastPollMs = 0;
  slot.lastOkMs = 0;
  slot.errors = 0;
  slot.consecutiveErrors = 0;
  return true;
}

void SensorRegistry::tryBegin(Slot& slot, uint32_t nowMs) {
  slot.started = slot.sensor->begin();
  slot.lastPollMs = nowMs;
  slot.consecutiveErrors = 0;
}

void SensorRegistry::beginAll(uint32_t nowMs) {
  for (int i = 0; i < count_; ++i) tryBegin(slots_[i], nowMs);
}

void SensorRegistry::pollAll(uint32_t nowMs) {
  for (int i = 0; i < count_; ++i) {
    Slot& slot = slots_[i];
    if (!slot.started) {
      if (nowMs - slot.lastPollMs >= kRetryMs) tryBegin(slot, nowMs);
      continue;
    }
    if (nowMs - slot.lastPollMs < slot.sensor->periodMs()) continue;
    slot.lastPollMs = nowMs;

    PollResult result = slot.sensor->poll(nowMs);
    if (result == PollResult::Ok) {
      slot.lastOkMs = nowMs;
      slot.everOk = true;
      slot.consecutiveErrors = 0;
    } else if (result == PollResult::Error) {
      slot.errors++;
      if (++slot.consecutiveErrors >= kMaxConsecutiveErrors) slot.started = false;  // set up again later
    }
  }
}

bool SensorRegistry::isFresh(const Slot& slot, uint32_t nowMs) const {
  return slot.everOk && nowMs - slot.lastOkMs <= slot.sensor->staleAfterMs();
}

void SensorRegistry::fillFrame(SensorFrame& frame, uint32_t nowMs) const {
  frame.clear();
  for (int i = 0; i < count_; ++i) {
    if (isFresh(slots_[i], nowMs)) slots_[i].sensor->fill(frame);
  }
}

const Sensor* SensorRegistry::sensorAt(int index) const {
  return (index >= 0 && index < count_) ? slots_[index].sensor : nullptr;
}

SensorState SensorRegistry::stateAt(int index, uint32_t nowMs) const {
  const Slot& slot = slots_[index];
  if (!slot.started && !slot.everOk) return SensorState::Missing;
  if (!slot.everOk) return SensorState::Waiting;
  return isFresh(slot, nowMs) ? SensorState::Ok : SensorState::Stale;
}

uint32_t SensorRegistry::errorsAt(int index) const {
  return (index >= 0 && index < count_) ? slots_[index].errors : 0;
}