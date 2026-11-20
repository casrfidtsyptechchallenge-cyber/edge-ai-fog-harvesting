#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mfrc522.h"
#include "sensor.h"

// The node's badge reader (M4): an RC522 polled 10 times a second. It is a Sensor, so the registry sets it
// up, looks for it again when it is missing and lists it in the info message; it fills no telemetry field.
// A tap is reported once per card: a card left on the reader is not reported again, and taps again only
// after it has been away for at least kGoneMs (a card on the edge of the field sometimes misses a poll).
class Rc522Reader : public Sensor {
 public:
  Rc522Reader(uint8_t csPin, uint8_t resetPin);
  const char* name() const override { return "RC522"; }
  bool begin() override;
  uint32_t periodMs() const override { return 100; }
  PollResult poll(uint32_t nowMs) override;
  void fill(SensorFrame&) const override {}

  // The newest tap, once: copies its UID (contract format, e.g. "04A1B2C3D4E5F6") and returns true.
  bool takeTap(char* uid, size_t cap);
  uint8_t chipVersion() const { return chip_.version(); }

 private:
  static const uint32_t kGoneMs = 500;
  static const int kCheckEvery = 10;  // polls between two checks that the chip still answers

  Mfrc522 chip_;
  bool failed_;  // the chip stopped answering: report errors until the registry sets it up again
  int pollsSinceCheck_;
  bool cardPresent_;
  char presentUid_[21];
  uint32_t lastSeenMs_;
  bool haveTap_;
  char tapUid_[21];
};