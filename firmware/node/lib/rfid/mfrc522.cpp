#include "mfrc522.h"

#include "board_io.h"
#include "iso14443a.h"

namespace {

// Chip registers (MFRC522 datasheet, section 9)
const uint8_t kCommandReg = 0x01;
const uint8_t kComIrqReg = 0x04;
const uint8_t kErrorReg = 0x06;
const uint8_t kFifoDataReg = 0x09;
const uint8_t kFifoLevelReg = 0x0A;
const uint8_t kBitFramingReg = 0x0D;
const uint8_t kCollReg = 0x0E;
const uint8_t kModeReg = 0x11;
const uint8_t kTxModeReg = 0x12;
const uint8_t kRxModeReg = 0x13;
const uint8_t kTxControlReg = 0x14;
const uint8_t kTxAskReg = 0x15;
const uint8_t kModWidthReg = 0x24;
const uint8_t kTModeReg = 0x2A;
const uint8_t kTPrescalerReg = 0x2B;
const uint8_t kTReloadRegH = 0x2C;
const uint8_t kTReloadRegL = 0x2D;
const uint8_t kVersionReg = 0x37;

// Chip commands
const uint8_t kIdle = 0x00;
const uint8_t kTransceive = 0x0C;
const uint8_t kSoftReset = 0x0F;

// Register bits
const uint8_t kPowerDown = 0x10;  // CommandReg
const uint8_t kStartSend = 0x80;  // BitFramingReg
const uint8_t kRxIrq = 0x20;      // ComIrqReg: a frame was received
const uint8_t kIdleIrq = 0x10;    // ComIrqReg: the command ended
const uint8_t kTimerIrq = 0x01;   // ComIrqReg: no answer before the timeout
const uint8_t kCollErr = 0x08;    // ErrorReg: two cards answered at once
const uint8_t kFrameErrors = 0x13;  // ErrorReg: buffer overflow, parity or protocol error

// Card commands (ISO/IEC 14443-3)
const uint8_t kWupa = 0x52;  // wake up every card, also one that was halted (7-bit frame)
const uint8_t kHlta = 0x50;
const uint8_t kSelectCommands[3] = {0x93, 0x95, 0x97};  // cascade levels 1, 2, 3
const uint8_t kCascadeTag = 0x88;                       // "more UID bytes at the next level"
const uint8_t kSakMore = 0x04;

const int kMaxWaitPolls = 1000;  // safety net; the chip's own 5 ms timer normally ends the wait first

}  // namespace

Mfrc522::Mfrc522(uint8_t csPin, uint8_t resetPin) : csPin_(csPin), resetPin_(resetPin), version_(0) {}

uint8_t Mfrc522::read(uint8_t reg) {
  const uint8_t out[2] = {static_cast<uint8_t>(0x80 | (reg << 1)), 0x00};
  uint8_t in[2] = {0x00, 0x00};
  if (!io_spi_transfer(csPin_, out, in, sizeof out)) return 0x00;
  return in[1];
}

void Mfrc522::write(uint8_t reg, uint8_t value) {
  const uint8_t out[2] = {static_cast<uint8_t>((reg << 1) & 0x7E), value};
  io_spi_transfer(csPin_, out, nullptr, sizeof out);
}

void Mfrc522::setBits(uint8_t reg, uint8_t mask) { write(reg, static_cast<uint8_t>(read(reg) | mask)); }

void Mfrc522::clearBits(uint8_t reg, uint8_t mask) { write(reg, static_cast<uint8_t>(read(reg) & ~mask)); }

bool Mfrc522::alive() {
  version_ = read(kVersionReg);
  return version_ != 0x00 && version_ != 0xFF;  // what an empty bus reads
}

bool Mfrc522::begin() {
  io_pin_output(resetPin_, true);  // the reset pin LOW keeps the chip powered down
  io_delay_us(1000);
  if (!alive()) return false;

  write(kCommandReg, kSoftReset);
  io_delay_us(50000);  // oscillator start-up: well above the datasheet's minimum
  for (int i = 0; i < 100 && (read(kCommandReg) & kPowerDown); ++i) io_delay_us(1000);

  write(kTxModeReg, 0x00);      // 106 kbit/s; the chip adds no CRC: this class does
  write(kRxModeReg, 0x00);
  write(kModWidthReg, 0x26);
  write(kTModeReg, 0x80);       // timer starts at the end of every transmission,
  write(kTPrescalerReg, 0xA9);  // ticks every 25 µs (13.56 MHz / 339),
  write(kTReloadRegH, 0x00);    // and gives up after 200 ticks = 5 ms without an answer
  write(kTReloadRegL, 0xC8);
  write(kTxAskReg, 0x40);       // 100 % ASK modulation
  write(kModeReg, 0x3D);        // CRC preset 0x6363, as ISO/IEC 14443 A wants
  setBits(kTxControlReg, 0x03);  // antenna on (TX1 and TX2)
  return alive();
}

Mfrc522::Status Mfrc522::transceive(const uint8_t* send, size_t sendLength, uint8_t txLastBits, uint8_t* back,
                                    size_t* backLength) {
  write(kCommandReg, kIdle);
  write(kComIrqReg, 0x7F);     // clear every interrupt flag
  write(kFifoLevelReg, 0x80);  // empty the FIFO
  for (size_t i = 0; i < sendLength; ++i) write(kFifoDataReg, send[i]);
  write(kBitFramingReg, txLastBits);  // bits to send from the last byte (0 = all 8)
  write(kCommandReg, kTransceive);
  setBits(kBitFramingReg, kStartSend);

  uint8_t irq = 0;
  for (int polls = 0;; ++polls) {
    irq = read(kComIrqReg);
    if (irq & (kRxIrq | kIdleIrq)) break;
    if ((irq & kTimerIrq) || polls >= kMaxWaitPolls) {
      clearBits(kBitFramingReg, kStartSend);
      return kNoAnswer;
    }
    io_delay_us(10);
  }
  clearBits(kBitFramingReg, kStartSend);

  uint8_t errors = read(kErrorReg);
  if (errors & (kFrameErrors | kCollErr)) return kError;
  size_t received = read(kFifoLevelReg);
  if (received > *backLength) return kError;
  for (size_t i = 0; i < received; ++i) back[i] = read(kFifoDataReg);
  *backLength = received;
  return kOk;
}

// One cascade level: anticollision (the card sends 4 UID bytes and their check byte), then select (we name
// those 4 bytes, the card answers with its SAK, which says whether more UID bytes follow).
bool Mfrc522::selectLevel(uint8_t command, uint8_t* bytes, uint8_t* sak) {
  uint8_t request[9] = {command, 0x20};
  uint8_t answer[5];
  size_t length = sizeof answer;
  if (transceive(request, 2, 0, answer, &length) != kOk || length != 5) return false;
  if (iso14443a::bcc(answer, 4) != answer[4]) return false;

  request[1] = 0x70;
  for (int i = 0; i < 5; ++i) request[2 + i] = answer[i];
  uint16_t crc = iso14443a::crcA(request, 7);
  request[7] = static_cast<uint8_t>(crc & 0xFF);
  request[8] = static_cast<uint8_t>(crc >> 8);
  uint8_t reply[3];
  length = sizeof reply;
  if (transceive(request, 9, 0, reply, &length) != kOk || length != 3) return false;
  if (iso14443a::crcA(reply, 1) != static_cast<uint16_t>(reply[1] | (reply[2] << 8))) return false;

  for (int i = 0; i < 4; ++i) bytes[i] = answer[i];
  *sak = reply[0];
  return true;
}

void Mfrc522::halt() {
  uint8_t frame[4] = {kHlta, 0x00};
  uint16_t crc = iso14443a::crcA(frame, 2);
  frame[2] = static_cast<uint8_t>(crc & 0xFF);
  frame[3] = static_cast<uint8_t>(crc >> 8);
  uint8_t unused[2];
  size_t length = sizeof unused;
  transceive(frame, sizeof frame, 0, unused, &length);  // a halted card does not answer: that is normal
}

int Mfrc522::readUid(uint8_t* uid) {
  const uint8_t wupa = kWupa;
  uint8_t atqa[2];
  size_t length = sizeof atqa;
  if (transceive(&wupa, 1, 7, atqa, &length) != kOk || length != 2) return 0;  // no card
  clearBits(kCollReg, 0x80);  // keep the received bits valid if two cards answer

  int count = 0;
  for (int level = 0; level < 3; ++level) {
    uint8_t bytes[4];
    uint8_t sak = 0;
    if (!selectLevel(kSelectCommands[level], bytes, &sak)) return 0;
    if (sak & kSakMore) {
      if (bytes[0] != kCascadeTag) return 0;
      for (int i = 1; i < 4; ++i) uid[count++] = bytes[i];
    } else {
      for (int i = 0; i < 4; ++i) uid[count++] = bytes[i];
      halt();
      return count;
    }
  }
  return 0;  // more than 3 levels: not ISO/IEC 14443 A
}