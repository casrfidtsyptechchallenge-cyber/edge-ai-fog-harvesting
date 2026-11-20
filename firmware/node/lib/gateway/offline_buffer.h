#pragma once
#include <stddef.h>
#include <stdint.h>

// Messages kept while the MQTT broker cannot be reached (contract rule 5), and sent oldest first when it
// is back. Fixed memory, no dynamic allocation: kSlots messages of at most kMaxPayload characters.
// When it is full, the oldest telemetry goes first, so events (state changes, faults, answers) last longer.
class OfflineBuffer {
 public:
  static const int kSlots = 64;
  static const size_t kMaxPayload = 640;  // a telemetry message is about 450 characters

  OfflineBuffer();

  // Stores one message (an event, or a telemetry if isEvent is false). False if it is too long to keep.
  bool push(const char* payload, bool isEvent);

  int count() const { return count_; }
  // The oldest message, or nullptr when empty.
  const char* frontPayload() const;
  bool frontIsEvent() const;
  void pop();  // removes the oldest
  void clear();

  uint32_t dropped() const { return dropped_; }  // messages lost because the buffer was full

 private:
  struct Slot {
    char payload[kMaxPayload];
    bool event;
  };
  int index(int i) const { return (head_ + i) % kSlots; }  // i-th oldest
  void removeAt(int i);

  Slot slots_[kSlots];
  int head_;
  int count_;
  uint32_t dropped_;
};