#pragma once
#include <stddef.h>
#include <stdint.h>

// Cuts the characters received on a serial port into lines. A line ends with '\n' or '\r' ("\r\n" works too)
// and empty lines are skipped. Backspace removes the last character (for people typing in a terminal).
// A line longer than the buffer is dropped whole and counted: half a command must never run.
class LineReader {
 public:
  // buffer: room for the longest line plus 1 (the line's end).
  LineReader(char* buffer, size_t cap) : buffer_(buffer), cap_(cap), length_(0), tooLong_(false), dropped_(0) {}

  // One received character (0 to 255). Returns the finished line, valid until the next call, or nullptr.
  char* feed(int c) {
    if (c == '\n' || c == '\r') {
      bool complete = length_ > 0 && !tooLong_;
      if (complete) buffer_[length_] = '\0';
      if (tooLong_) dropped_++;
      length_ = 0;
      tooLong_ = false;
      return complete ? buffer_ : nullptr;
    }
    if (c == 0x08 || c == 0x7F) {  // backspace, delete
      if (length_ > 0 && !tooLong_) length_--;
      return nullptr;
    }
    if (c < ' ' || c > 0xFF || tooLong_) return nullptr;  // other control characters are noise
    if (length_ + 1 >= cap_) {
      tooLong_ = true;
      return nullptr;
    }
    buffer_[length_++] = static_cast<char>(c);
    return nullptr;
  }

  uint32_t dropped() const { return dropped_; }  // lines dropped because they were too long

 private:
  char* buffer_;
  size_t cap_;
  size_t length_;
  bool tooLong_;
  uint32_t dropped_;
};