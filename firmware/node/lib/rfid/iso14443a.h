#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure helpers for ISO/IEC 14443 A, the card standard the RC522 reads: no hardware, tested on the PC
// (test/test_rfid).
namespace iso14443a {

// CRC_A (ISO/IEC 14443-3, annex B), sent low byte first after most commands.
inline uint16_t crcA(const uint8_t* data, size_t length) {
  uint16_t crc = 0x6363;
  for (size_t i = 0; i < length; ++i) {
    uint8_t b = static_cast<uint8_t>(data[i] ^ (crc & 0xFF));
    b = static_cast<uint8_t>(b ^ (b << 4));
    crc = static_cast<uint16_t>((crc >> 8) ^ (b << 8) ^ (b << 3) ^ (b >> 4));
  }
  return crc;
}

// Check byte of an anticollision answer: the XOR of the 4 UID bytes before it.
inline uint8_t bcc(const uint8_t* data, size_t length) {
  uint8_t x = 0;
  for (size_t i = 0; i < length; ++i) x ^= data[i];
  return x;
}

// UID bytes as the contract writes them: upper-case hex, two characters per byte (04A1B2C3...).
// False if `cap` is too small.
inline bool uidToHex(const uint8_t* uid, size_t length, char* out, size_t cap) {
  static const char kDigits[] = "0123456789ABCDEF";
  if (cap < 2 * length + 1) return false;
  for (size_t i = 0; i < length; ++i) {
    out[2 * i] = kDigits[uid[i] >> 4];
    out[2 * i + 1] = kDigits[uid[i] & 0x0F];
  }
  out[2 * length] = '\0';
  return true;
}

}  // namespace iso14443a