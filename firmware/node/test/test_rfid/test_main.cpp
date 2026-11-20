// Tests for the RFID code (lib/rfid): ISO/IEC 14443 A helpers, the RC522 driver against a simulated chip
// and card, the tap logic, the badge list and SYNC_ALLOWLIST. They run on the PC: pio test -e native
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "allowlist_sync.h"
#include "badge_list.h"
#include "board_io.h"
#include "iso14443a.h"
#include "mfrc522.h"
#include "node_event.h"
#include "rc522_reader.h"
#include "sensor_registry.h"

const uint8_t kCs = 10;
const uint8_t kReset = 11;

// ---------------------------------------------------------------- a simulated RC522 with one card

namespace fake {

// Chip registers used here
const uint8_t kCommandReg = 0x01, kComIrqReg = 0x04, kErrorReg = 0x06, kFifoDataReg = 0x09, kFifoLevelReg = 0x0A,
              kBitFramingReg = 0x0D, kTxControlReg = 0x14, kVersionReg = 0x37;

enum CardState { kIdle, kReady, kActive, kHalt };

bool chipPresent;  // false: nothing on the SPI bus, every byte reads 0xFF
uint8_t regs[64];
uint8_t fifo[64];
int fifoCount;
int fifoRead;
bool resetPinHigh;

uint8_t uid[10];  // the card in the field
int uidLength;
bool cardPresent;
CardState state;
int level;         // cascade level being selected (0..2)
bool twoCards;     // a second card answers at the same time: collision
bool garbled;      // the anticollision answer has a wrong check byte
int halts;

void reset() {
  chipPresent = true;
  memset(regs, 0, sizeof regs);
  fifoCount = fifoRead = 0;
  resetPinHigh = false;
  cardPresent = false;
  state = kIdle;
  level = 0;
  twoCards = garbled = false;
  halts = 0;
}

void placeCard(const uint8_t* bytes, int length) {
  memcpy(uid, bytes, length);
  uidLength = length;
  cardPresent = true;
  state = kIdle;  // a card entering the field powers up idle
}

void removeCard() {
  cardPresent = false;
  state = kIdle;
}

int levels() { return uidLength == 4 ? 1 : (uidLength == 7 ? 2 : 3); }

// The 4 bytes the card sends at one cascade level: cascade tag + 3 UID bytes, or the last 4 UID bytes.
void levelBytes(int lvl, uint8_t out[4]) {
  if (lvl == levels() - 1) {
    memcpy(out, uid + uidLength - 4, 4);
  } else {
    out[0] = 0x88;
    memcpy(out + 1, uid + 3 * lvl, 3);
  }
}

void respond(const uint8_t* bytes, int length) {
  memcpy(fifo, bytes, length);
  fifoCount = length;
  fifoRead = 0;
  regs[kComIrqReg] |= 0x30;  // received + idle
}

void noAnswer() {
  fifoCount = fifoRead = 0;
  regs[kComIrqReg] |= 0x01;  // the timer ran out
}

void transceive(int txLastBits) {
  uint8_t frame[64];
  int n = fifoCount;
  memcpy(frame, fifo, n);
  fifoCount = fifoRead = 0;
  regs[kErrorReg] = 0;
  if (!cardPresent) return noAnswer();

  if (txLastBits == 7 && n == 1 && (frame[0] == 0x52 || frame[0] == 0x26)) {  // WUPA / REQA
    bool wakes = state == kIdle || (frame[0] == 0x52 && state == kHalt);
    if (!wakes) return noAnswer();
    state = kReady;
    level = 0;
    const uint8_t atqa[2] = {static_cast<uint8_t>(uidLength == 4 ? 0x04 : 0x44), 0x00};
    return respond(atqa, 2);
  }
  if (n == 4 && frame[0] == 0x50 && frame[1] == 0x00) {  // HLTA
    if (iso14443a::crcA(frame, 2) == (frame[2] | (frame[3] << 8))) {
      state = kHalt;
      halts++;
    }
    return noAnswer();  // a card never answers HLTA
  }
  if (state != kReady || n < 2 || frame[0] != 0x93 + 2 * level) return noAnswer();

  uint8_t bytes[4];
  levelBytes(level, bytes);
  if (n == 2 && frame[1] == 0x20) {  // anticollision
    uint8_t answer[5] = {bytes[0], bytes[1], bytes[2], bytes[3], iso14443a::bcc(bytes, 4)};
    if (garbled) answer[4] ^= 0x5A;
    if (twoCards) regs[kErrorReg] = 0x08;
    return respond(answer, 5);
  }
  if (n == 9 && frame[1] == 0x70) {  // select
    bool sameBytes = memcmp(frame + 2, bytes, 4) == 0 && frame[6] == iso14443a::bcc(bytes, 4);
    if (!sameBytes || iso14443a::crcA(frame, 7) != (frame[7] | (frame[8] << 8))) return noAnswer();
    bool last = level == levels() - 1;
    uint8_t reply[3] = {static_cast<uint8_t>(last ? 0x00 : 0x04), 0, 0};
    uint16_t crc = iso14443a::crcA(reply, 1);
    reply[1] = static_cast<uint8_t>(crc & 0xFF);
    reply[2] = static_cast<uint8_t>(crc >> 8);
    if (last) state = kActive; else level++;
    return respond(reply, 3);
  }
  noAnswer();
}

void writeReg(uint8_t reg, uint8_t value) {
  switch (reg) {
    case kCommandReg:
      if (value == 0x0F) {  // soft reset
        memset(regs, 0, sizeof regs);
        fifoCount = fifoRead = 0;
      }
      regs[reg] = value;
      break;
    case kComIrqReg:  // bit 7 set: set the marked bits; clear: clear them
      if (value & 0x80) regs[reg] |= value & 0x7F; else regs[reg] &= static_cast<uint8_t>(~value);
      break;
    case kFifoLevelReg:
      if (value & 0x80) fifoCount = fifoRead = 0;
      break;
    case kFifoDataReg:
      if (fifoCount < 64) fifo[fifoCount++] = value;
      break;
    case kBitFramingReg:
      regs[reg] = value;
      if ((value & 0x80) && (regs[kCommandReg] & 0x0F) == 0x0C) transceive(value & 0x07);
      break;
    default:
      regs[reg] = value;
  }
}

uint8_t readReg(uint8_t reg) {
  if (reg == kFifoDataReg) return fifoRead < fifoCount ? fifo[fifoRead++] : 0;
  if (reg == kFifoLevelReg) return static_cast<uint8_t>(fifoCount - fifoRead);
  if (reg == kVersionReg) return 0x92;
  return regs[reg];
}

}  // namespace fake

extern "C" {
bool io_spi_transfer(uint8_t csPin, const uint8_t* out, uint8_t* in, size_t length) {
  if (!fake::chipPresent || csPin != kCs) {
    if (in) memset(in, 0xFF, length);
    return true;
  }
  bool reading = (out[0] & 0x80) != 0;
  uint8_t reg = (out[0] >> 1) & 0x3F;
  if (in) in[0] = 0;
  for (size_t i = 1; i < length; ++i) {
    if (reading) {
      uint8_t value = fake::readReg(reg);
      if (in) in[i] = value;
    } else {
      fake::writeReg(reg, out[i]);
    }
  }
  return true;
}
void io_pin_output(uint8_t pin, bool high) {
  if (pin == kReset) fake::resetPinHigh = high;
}
void io_pin_write(uint8_t pin, bool high) { io_pin_output(pin, high); }
void io_delay_us(uint32_t) {}
// Not used by lib/rfid; defined so the test links however the libraries are bundled.
void io_pin_input_pullup(uint8_t) {}
bool io_pin_read(uint8_t) { return true; }
bool io_analog_mv(uint8_t, float*) { return false; }
bool io_i2c_write(uint8_t, const uint8_t*, size_t) { return false; }
bool io_i2c_write_read(uint8_t, const uint8_t*, size_t, uint8_t*, size_t) { return false; }
}

const uint8_t kUid4[4] = {0x04, 0xA1, 0xB2, 0xC3};
const uint8_t kUid7[7] = {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
const uint8_t kUid10[10] = {0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};

void setUp() { fake::reset(); }
void tearDown() {}

// Polls the reader every 100 ms from `fromMs` to `toMs` (inclusive) and counts the taps it reports.
int pollTaps(Rc522Reader& reader, uint32_t fromMs, uint32_t toMs, char* lastUid) {
  int taps = 0;
  for (uint32_t t = fromMs; t <= toMs; t += 100) {
    reader.poll(t);
    if (reader.takeTap(lastUid, 21)) taps++;
  }
  return taps;
}

// ---------------------------------------------------------------- ISO/IEC 14443 A helpers

void test_crc_a_matches_the_standard_examples() {
  const uint8_t zeros[2] = {0x00, 0x00};
  const uint8_t example[2] = {0x12, 0x34};
  const uint8_t halt[2] = {0x50, 0x00};
  TEST_ASSERT_EQUAL_HEX16(0x1EA0, iso14443a::crcA(zeros, 2));    // sent as A0 1E (ISO/IEC 14443-3 annex B)
  TEST_ASSERT_EQUAL_HEX16(0xCF26, iso14443a::crcA(example, 2));  // sent as 26 CF
  TEST_ASSERT_EQUAL_HEX16(0xCD57, iso14443a::crcA(halt, 2));     // the well-known HLTA frame 50 00 57 CD
}

void test_uid_is_written_as_upper_case_hex() {
  char text[21];
  TEST_ASSERT_TRUE(iso14443a::uidToHex(kUid4, 4, text, sizeof text));
  TEST_ASSERT_EQUAL_STRING("04A1B2C3", text);
  TEST_ASSERT_TRUE(iso14443a::uidToHex(kUid10, 10, text, sizeof text));
  TEST_ASSERT_EQUAL_STRING("04010203040506070809", text);
  TEST_ASSERT_FALSE(iso14443a::uidToHex(kUid10, 10, text, 20));  // no room for the end of the text
  TEST_ASSERT_EQUAL_HEX8(0x04 ^ 0xA1 ^ 0xB2 ^ 0xC3, iso14443a::bcc(kUid4, 4));
}

// ---------------------------------------------------------------- RC522 driver

void test_begin_fails_when_no_chip_answers() {
  fake::chipPresent = false;
  Mfrc522 chip(kCs, kReset);
  TEST_ASSERT_FALSE(chip.begin());
}

void test_begin_wakes_the_chip_and_turns_the_antenna_on() {
  Mfrc522 chip(kCs, kReset);
  TEST_ASSERT_TRUE(chip.begin());
  TEST_ASSERT_TRUE(fake::resetPinHigh);
  TEST_ASSERT_EQUAL_HEX8(0x92, chip.version());
  TEST_ASSERT_EQUAL_HEX8(0x03, fake::regs[fake::kTxControlReg] & 0x03);
}

void test_reads_a_4_byte_uid_and_halts_the_card() {
  Mfrc522 chip(kCs, kReset);
  chip.begin();
  fake::placeCard(kUid4, 4);
  uint8_t uid[10];
  TEST_ASSERT_EQUAL_INT(4, chip.readUid(uid));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(kUid4, uid, 4);
  TEST_ASSERT_EQUAL_INT(fake::kHalt, fake::state);
}

void test_reads_7_and_10_byte_uids() {
  Mfrc522 chip(kCs, kReset);
  chip.begin();
  uint8_t uid[10];
  fake::placeCard(kUid7, 7);
  TEST_ASSERT_EQUAL_INT(7, chip.readUid(uid));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(kUid7, uid, 7);
  fake::placeCard(kUid10, 10);
  TEST_ASSERT_EQUAL_INT(10, chip.readUid(uid));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(kUid10, uid, 10);
}

void test_reads_nothing_without_a_card() {
  Mfrc522 chip(kCs, kReset);
  chip.begin();
  uint8_t uid[10];
  TEST_ASSERT_EQUAL_INT(0, chip.readUid(uid));
}

// WUPA also wakes a halted card, so a card left on the reader is seen at every poll.
void test_a_card_left_on_the_reader_is_found_again() {
  Mfrc522 chip(kCs, kReset);
  chip.begin();
  fake::placeCard(kUid4, 4);
  uint8_t uid[10];
  TEST_ASSERT_EQUAL_INT(4, chip.readUid(uid));
  TEST_ASSERT_EQUAL_INT(4, chip.readUid(uid));
  TEST_ASSERT_EQUAL_INT(2, fake::halts);
}

void test_two_cards_or_a_garbled_answer_read_nothing() {
  Mfrc522 chip(kCs, kReset);
  chip.begin();
  uint8_t uid[10];
  fake::placeCard(kUid4, 4);
  fake::twoCards = true;
  TEST_ASSERT_EQUAL_INT(0, chip.readUid(uid));
  fake::twoCards = false;
  fake::garbled = true;
  fake::removeCard();
  fake::placeCard(kUid4, 4);
  TEST_ASSERT_EQUAL_INT(0, chip.readUid(uid));
}

// ---------------------------------------------------------------- taps

void test_a_card_held_on_the_reader_is_one_tap() {
  Rc522Reader reader(kCs, kReset);
  TEST_ASSERT_TRUE(reader.begin());
  char uid[21];
  TEST_ASSERT_EQUAL_INT(0, pollTaps(reader, 0, 500, uid));
  fake::placeCard(kUid7, 7);
  TEST_ASSERT_EQUAL_INT(1, pollTaps(reader, 600, 3000, uid));
  TEST_ASSERT_EQUAL_STRING("04112233445566", uid);
}

void test_a_card_taps_again_after_being_away() {
  Rc522Reader reader(kCs, kReset);
  reader.begin();
  char uid[21];
  fake::placeCard(kUid4, 4);
  TEST_ASSERT_EQUAL_INT(1, pollTaps(reader, 0, 500, uid));
  fake::removeCard();
  pollTaps(reader, 600, 1200, uid);  // away 700 ms
  fake::placeCard(kUid4, 4);
  TEST_ASSERT_EQUAL_INT(1, pollTaps(reader, 1300, 2000, uid));
}

void test_a_short_dropout_is_not_a_new_tap() {
  Rc522Reader reader(kCs, kReset);
  reader.begin();
  char uid[21];
  fake::placeCard(kUid4, 4);
  TEST_ASSERT_EQUAL_INT(1, pollTaps(reader, 0, 500, uid));
  fake::removeCard();
  pollTaps(reader, 600, 800, uid);  // missed 3 polls on the edge of the field
  fake::placeCard(kUid4, 4);
  TEST_ASSERT_EQUAL_INT(0, pollTaps(reader, 900, 2000, uid));
}

void test_another_card_is_a_new_tap_at_once() {
  Rc522Reader reader(kCs, kReset);
  reader.begin();
  char uid[21];
  fake::placeCard(kUid4, 4);
  TEST_ASSERT_EQUAL_INT(1, pollTaps(reader, 0, 300, uid));
  fake::placeCard(kUid7, 7);
  TEST_ASSERT_EQUAL_INT(1, pollTaps(reader, 400, 700, uid));
  TEST_ASSERT_EQUAL_STRING("04112233445566", uid);
}

void test_an_unplugged_reader_is_set_up_again_by_the_registry() {
  Rc522Reader reader(kCs, kReset);
  SensorRegistry registry;
  registry.add(&reader);
  registry.beginAll(0);
  TEST_ASSERT_TRUE(registry.stateAt(0, 0) == SensorState::Waiting);
  registry.pollAll(100);
  TEST_ASSERT_TRUE(registry.stateAt(0, 100) == SensorState::Ok);

  fake::chipPresent = false;  // unplugged: noticed within 10 polls, then an error at every poll
  for (uint32_t t = 200; t <= 3000; t += 100) registry.pollAll(t);
  TEST_ASSERT_TRUE(registry.errorsAt(0) >= 10);

  fake::chipPresent = true;  // plugged back: the registry runs begin() again within 5 s
  for (uint32_t t = 3100; t <= 9000; t += 100) registry.pollAll(t);
  TEST_ASSERT_TRUE(registry.stateAt(0, 9000) == SensorState::Ok);
  fake::placeCard(kUid4, 4);
  registry.pollAll(9100);
  char uid[21];
  TEST_ASSERT_TRUE(reader.takeTap(uid, sizeof uid));
}

// ---------------------------------------------------------------- badge list

void test_a_badge_not_on_the_list_is_unknown() {
  BadgeList list;
  TEST_ASSERT_TRUE(list.set("04A1B2C3", AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_OPERATOR, list.roleOf("04A1B2C3"));
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_UNKNOWN, list.roleOf("04A1B2C4"));
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_UNKNOWN, list.roleOf(nullptr));
}

void test_set_changes_the_role_of_a_badge_already_listed() {
  BadgeList list;
  list.set("04A1B2C3", AEROION_ROLE_OPERATOR);
  list.set("04A1B2C3", AEROION_ROLE_TECHNICIAN);
  TEST_ASSERT_EQUAL_INT(1, list.count());
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_TECHNICIAN, list.roleOf("04A1B2C3"));
}

void test_set_refuses_uids_outside_the_contract() {
  BadgeList list;
  TEST_ASSERT_FALSE(list.set("04a1b2c3", AEROION_ROLE_OPERATOR));  // lower case
  TEST_ASSERT_FALSE(list.set("04A1B2", AEROION_ROLE_OPERATOR));    // too short
  TEST_ASSERT_FALSE(list.set(nullptr, AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL_INT(0, list.count());
}

void test_the_list_holds_64_badges() {
  BadgeList list;
  char uid[9];
  for (int i = 0; i < BadgeList::kMaxBadges; ++i) {
    snprintf(uid, sizeof uid, "0400%04X", i);
    TEST_ASSERT_TRUE(list.set(uid, AEROION_ROLE_OPERATOR));
  }
  TEST_ASSERT_FALSE(list.set("04FFFFFF", AEROION_ROLE_OPERATOR));
  TEST_ASSERT_TRUE(list.remove("04000000"));
  TEST_ASSERT_TRUE(list.set("04FFFFFF", AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_UNKNOWN, list.roleOf("04000000"));
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_OPERATOR, list.roleOf("0400003F"));
}

void test_role_names_follow_the_contract() {
  aeroion_role_t role;
  TEST_ASSERT_TRUE(badgeRoleFromName("technician", &role));
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_TECHNICIAN, role);
  TEST_ASSERT_FALSE(badgeRoleFromName("Technician", &role));
  TEST_ASSERT_FALSE(badgeRoleFromName(nullptr, &role));
}

// ---------------------------------------------------------------- SYNC_ALLOWLIST

bool sync(BadgeList& list, const char* json, char* reason) {
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, json) == DeserializationError::Ok);
  return applySyncAllowlist(doc.as<JsonObjectConst>(), list, reason, 65);
}

void test_sync_replaces_the_whole_list() {
  BadgeList list;
  list.set("0E0E0E0E", AEROION_ROLE_TECHNICIAN);  // from the settings
  char reason[65];
  TEST_ASSERT_TRUE(sync(list,
                        "{\"version\":3,\"entries\":[{\"uid\":\"04A1B2C3\",\"role\":\"operator\"},"
                        "{\"uid\":\"04112233445566\",\"role\":\"technician\"}]}",
                        reason));
  TEST_ASSERT_EQUAL_STRING("", reason);
  TEST_ASSERT_EQUAL_INT(2, list.count());
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_OPERATOR, list.roleOf("04A1B2C3"));
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_TECHNICIAN, list.roleOf("04112233445566"));
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_UNKNOWN, list.roleOf("0E0E0E0E"));
  TEST_ASSERT_TRUE(list.fromStation());
  TEST_ASSERT_EQUAL_UINT32(3, list.version());
}

void test_sync_with_one_bad_entry_changes_nothing() {
  BadgeList list;
  list.set("0E0E0E0E", AEROION_ROLE_TECHNICIAN);
  char reason[65];
  TEST_ASSERT_FALSE(sync(list,
                         "{\"version\":1,\"entries\":[{\"uid\":\"04A1B2C3\",\"role\":\"operator\"},"
                         "{\"uid\":\"04A1B2C4\",\"role\":\"pilot\"}]}",
                         reason));
  TEST_ASSERT_EQUAL_STRING("entry 1: unknown role", reason);
  TEST_ASSERT_FALSE(sync(list, "{\"version\":1,\"entries\":[{\"uid\":\"04a1b2c3\",\"role\":\"operator\"}]}", reason));
  TEST_ASSERT_EQUAL_STRING("entry 0: uid must be 8 to 20 upper-case hex characters", reason);
  TEST_ASSERT_EQUAL_INT(1, list.count());
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_TECHNICIAN, list.roleOf("0E0E0E0E"));
  TEST_ASSERT_FALSE(list.fromStation());
}

void test_sync_refuses_more_than_64_entries() {
  JsonDocument doc;
  doc["version"] = 1;
  JsonArray entries = doc["entries"].to<JsonArray>();
  char uid[9];
  for (int i = 0; i < 65; ++i) {
    snprintf(uid, sizeof uid, "0400%04X", i);
    JsonObject entry = entries.add<JsonObject>();
    entry["uid"] = uid;  // copied: uid is a local buffer
    entry["role"] = "operator";
  }
  BadgeList list;
  char reason[65];
  TEST_ASSERT_FALSE(applySyncAllowlist(doc.as<JsonObjectConst>(), list, reason, sizeof reason));
  TEST_ASSERT_EQUAL_STRING("more than 64 entries", reason);
}

void test_sync_refuses_an_older_list_but_accepts_a_resend() {
  BadgeList list;
  char reason[65];
  TEST_ASSERT_TRUE(sync(list, "{\"version\":5,\"entries\":[]}", reason));
  TEST_ASSERT_TRUE(sync(list, "{\"version\":5,\"entries\":[{\"uid\":\"04A1B2C3\",\"role\":\"admin\"}]}", reason));
  TEST_ASSERT_FALSE(sync(list, "{\"version\":4,\"entries\":[]}", reason));
  TEST_ASSERT_EQUAL_STRING("older list: version 4, node has 5", reason);
  TEST_ASSERT_EQUAL_INT(AEROION_ROLE_ADMIN, list.roleOf("04A1B2C3"));
}

void test_sync_needs_a_version_and_entries() {
  BadgeList list;
  char reason[65];
  TEST_ASSERT_FALSE(sync(list, "{\"entries\":[]}", reason));
  TEST_ASSERT_FALSE(sync(list, "{\"version\":-1,\"entries\":[]}", reason));
  TEST_ASSERT_FALSE(sync(list, "{\"version\":1.5,\"entries\":[]}", reason));
  TEST_ASSERT_EQUAL_STRING("version must be a whole number from 0", reason);
  TEST_ASSERT_FALSE(sync(list, "{\"version\":1}", reason));
  TEST_ASSERT_EQUAL_STRING("entries missing", reason);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc_a_matches_the_standard_examples);
  RUN_TEST(test_uid_is_written_as_upper_case_hex);
  RUN_TEST(test_begin_fails_when_no_chip_answers);
  RUN_TEST(test_begin_wakes_the_chip_and_turns_the_antenna_on);
  RUN_TEST(test_reads_a_4_byte_uid_and_halts_the_card);
  RUN_TEST(test_reads_7_and_10_byte_uids);
  RUN_TEST(test_reads_nothing_without_a_card);
  RUN_TEST(test_a_card_left_on_the_reader_is_found_again);
  RUN_TEST(test_two_cards_or_a_garbled_answer_read_nothing);
  RUN_TEST(test_a_card_held_on_the_reader_is_one_tap);
  RUN_TEST(test_a_card_taps_again_after_being_away);
  RUN_TEST(test_a_short_dropout_is_not_a_new_tap);
  RUN_TEST(test_another_card_is_a_new_tap_at_once);
  RUN_TEST(test_an_unplugged_reader_is_set_up_again_by_the_registry);
  RUN_TEST(test_a_badge_not_on_the_list_is_unknown);
  RUN_TEST(test_set_changes_the_role_of_a_badge_already_listed);
  RUN_TEST(test_set_refuses_uids_outside_the_contract);
  RUN_TEST(test_the_list_holds_64_badges);
  RUN_TEST(test_role_names_follow_the_contract);
  RUN_TEST(test_sync_replaces_the_whole_list);
  RUN_TEST(test_sync_with_one_bad_entry_changes_nothing);
  RUN_TEST(test_sync_refuses_more_than_64_entries);
  RUN_TEST(test_sync_refuses_an_older_list_but_accepts_a_resend);
  RUN_TEST(test_sync_needs_a_version_and_entries);
  return UNITY_END();
}