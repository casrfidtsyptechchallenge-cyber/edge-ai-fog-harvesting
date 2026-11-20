#include "rc522_reader.h"

#include <string.h>

#include "iso14443a.h"

namespace {
void copyUid(char* out, size_t cap, const char* uid) {
  strncpy(out, uid, cap - 1);
  out[cap - 1] = '\0';
}
}  // namespace

Rc522Reader::Rc522Reader(uint8_t csPin, uint8_t resetPin)
    : chip_(csPin, resetPin), failed_(false), pollsSinceCheck_(0), cardPresent_(false), lastSeenMs_(0),
      haveTap_(false) {
  presentUid_[0] = '\0';
  tapUid_[0] = '\0';
}

bool Rc522Reader::begin() {
  failed_ = false;
  pollsSinceCheck_ = 0;
  cardPresent_ = false;
  return chip_.begin();
}

PollResult Rc522Reader::poll(uint32_t nowMs) {
  if (failed_) return PollResult::Error;
  if (++pollsSinceCheck_ >= kCheckEvery) {
    pollsSinceCheck_ = 0;
    if (!chip_.alive()) {
      failed_ = true;  // unplugged or reset: after 10 errors in a row the registry runs begin() again
      return PollResult::Error;
    }
  }

  uint8_t bytes[Mfrc522::kMaxUidBytes];
  int length = chip_.readUid(bytes);
  if (length > 0) {
    char uid[21];
    iso14443a::uidToHex(bytes, static_cast<size_t>(length), uid, sizeof uid);
    if (!cardPresent_ || strcmp(uid, presentUid_) != 0) {  // a new card, or one that had left
      copyUid(presentUid_, sizeof presentUid_, uid);
      copyUid(tapUid_, sizeof tapUid_, uid);
      haveTap_ = true;
    }
    cardPresent_ = true;
    lastSeenMs_ = nowMs;
  } else if (cardPresent_ && nowMs - lastSeenMs_ >= kGoneMs) {
    cardPresent_ = false;
  }
  return PollResult::Ok;
}

bool Rc522Reader::takeTap(char* uid, size_t cap) {
  if (!haveTap_ || cap == 0) return false;
  copyUid(uid, cap, tapUid_);
  haveTap_ = false;
  return true;
}