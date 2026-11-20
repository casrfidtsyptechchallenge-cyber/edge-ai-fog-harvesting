#pragma once
#include <stddef.h>
#include <stdint.h>

// NXP MFRC522, the chip on the common "RC522" module: a 13.56 MHz reader for ISO/IEC 14443 A cards
// (MIFARE Classic, MIFARE Ultralight, NTAG21x). The chip only sends and receives radio frames; this class
// runs the card protocol: wake any card in the field (WUPA), read its UID one cascade level at a time
// (anticollision + select), then put it to sleep (HLTA) so the next poll can tell whether it is still there.
// The chip is reached by SPI through board_io.h.
class Mfrc522 {
 public:
  static const int kMaxUidBytes = 10;

  Mfrc522(uint8_t csPin, uint8_t resetPin);

  bool begin();  // wakes and sets up the chip, antenna on; false if no chip answers
  bool alive();  // the chip still answers with a valid version number

  // Looks for one card. Returns the UID length (4, 7 or 10 bytes), or 0 when there is no card, when two
  // cards answer at once, or when an answer is garbled.
  int readUid(uint8_t* uid);

  uint8_t version() const { return version_; }  // 0x91 or 0x92 for NXP chips; clones differ

 private:
  enum Status { kOk, kNoAnswer, kError };

  uint8_t read(uint8_t reg);
  void write(uint8_t reg, uint8_t value);
  void setBits(uint8_t reg, uint8_t mask);
  void clearBits(uint8_t reg, uint8_t mask);
  Status transceive(const uint8_t* send, size_t sendLength, uint8_t txLastBits, uint8_t* back, size_t* backLength);
  bool selectLevel(uint8_t command, uint8_t* bytes, uint8_t* sak);
  void halt();

  uint8_t csPin_;
  uint8_t resetPin_;
  uint8_t version_;
};