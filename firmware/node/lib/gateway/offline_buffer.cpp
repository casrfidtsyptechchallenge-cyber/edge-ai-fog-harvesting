#include "offline_buffer.h"

#include <string.h>

OfflineBuffer::OfflineBuffer() : head_(0), count_(0), dropped_(0) {}

bool OfflineBuffer::push(const char* payload, bool isEvent) {
  size_t length = strlen(payload);
  if (length >= kMaxPayload) {
    dropped_++;
    return false;
  }
  if (count_ == kSlots) {  // full: make room, oldest telemetry first
    int victim = 0;
    for (int i = 0; i < count_; ++i) {
      if (!slots_[index(i)].event) {
        victim = i;
        break;
      }
    }
    removeAt(victim);
    dropped_++;
  }
  Slot& slot = slots_[index(count_)];
  memcpy(slot.payload, payload, length + 1);
  slot.event = isEvent;
  count_++;
  return true;
}

const char* OfflineBuffer::frontPayload() const { return count_ > 0 ? slots_[head_].payload : nullptr; }

bool OfflineBuffer::frontIsEvent() const { return count_ > 0 && slots_[head_].event; }

void OfflineBuffer::pop() {
  if (count_ == 0) return;
  head_ = (head_ + 1) % kSlots;
  count_--;
}

void OfflineBuffer::clear() {
  head_ = 0;
  count_ = 0;
}

// Removes the i-th oldest message: the older ones move up by one place.
void OfflineBuffer::removeAt(int i) {
  for (int k = i; k > 0; --k) slots_[index(k)] = slots_[index(k - 1)];
  pop();
}