#include "node_console.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "board_io.h"
#include "node_event.h"  // isContractUid()

namespace {

const char* kTestOperatorUid = "0A0B0C0D";    // SW2 (on the badge list in mcx_node_config.h)
const char* kTestTechnicianUid = "0E0E0E0E";  // SW3

const char* kHelp =
    "# badges:  tap <UID>            (as if that card was read; SW2 / SW3 or op / tech: test badges)\r\n"
    "#          badges | allow <UID> <role> | forget <UID>   (role: operator, technician, admin, household)\r\n"
    "# show:    state | sensors      (sensors: each sensor's state and raw reading)\r\n"
    "# check:   i2c                  (lists the I2C devices that answer; 0x48 is on the board)\r\n"
    "#          link                 (station link: up or down, clock, commands received and answered)\r\n"
    "# station: paste one JSON command line, e.g. from: python -m station.badges sync --node n01 --print\r\n"
    "# adjust:  zero                 (measure the collector-current zero again, HV off only)\r\n"
    "# arc AI:  ai | ai auto|model|rules   (the arc predictor, M5a: status, or which one decides)\r\n"
    "#          wave | waves <s> | waves off   (send the next window, or one every s seconds, as a wave message)\r\n";

void upperCase(char* text) {
  for (char* p = text; *p; ++p) *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
}

}  // namespace

bool NodeConsole::Button::press(bool down) {
  count_ = (down == raw_) ? (count_ < 3 ? count_ + 1 : 3) : 0;
  raw_ = down;
  if (count_ == 3 && raw_ != stable_) {
    stable_ = raw_;
    return stable_;
  }
  return false;
}

NodeConsole::NodeConsole(SafetyCore& safety, SensorRegistry& registry, NodeSensors& sensors, BadgeList& badges,
                         NodeLink& link, PrintFn print)
    : safety_(safety), registry_(registry), sensors_(sensors), badges_(badges), link_(link), print_(print) {}

void NodeConsole::buttons(bool sw2Down, bool sw3Down) {
  if (sw2_.press(sw2Down)) tapCard(kTestOperatorUid);
  if (sw3_.press(sw3Down)) tapCard(kTestTechnicianUid);
}

void NodeConsole::tapCard(const char* uid) {
  char text[96];
  aeroion_role_t role = badges_.roleOf(uid);
  aeroion_badge_result_t result = safety_.badge(uid, role);
  snprintf(text, sizeof text, "# badge %s (%s): %s\r\n", uid, aeroion_role_name(role),
           aeroion_badge_result_name(result));
  print_(text);
  printState();
}

void NodeConsole::run(char* line, uint32_t nowMs) {
  char* command = strtok(line, " ");
  char* arg1 = strtok(nullptr, " ");
  char* arg2 = strtok(nullptr, " ");
  if (command == nullptr) return;
  if (strcmp(command, "op") == 0) {
    tapCard(kTestOperatorUid);
  } else if (strcmp(command, "tech") == 0) {
    tapCard(kTestTechnicianUid);
  } else if (strcmp(command, "tap") == 0 && arg1 != nullptr) {
    tapTyped(arg1);
  } else if (strcmp(command, "badges") == 0) {
    printBadges();
  } else if (strcmp(command, "allow") == 0 && arg1 != nullptr && arg2 != nullptr) {
    allow(arg1, arg2);
  } else if (strcmp(command, "forget") == 0 && arg1 != nullptr) {
    forget(arg1);
  } else if (strcmp(command, "state") == 0) {
    printState();
  } else if (strcmp(command, "sensors") == 0) {
    printSensors(nowMs);
  } else if (strcmp(command, "i2c") == 0) {
    scanI2c();
  } else if (strcmp(command, "link") == 0) {
    link_.printStatus(print_, nowMs);
  } else if (strcmp(command, "zero") == 0) {
    zero();
  } else {
    print_(kHelp);
  }
}

void NodeConsole::tapTyped(char* uid) {
  upperCase(uid);
  if (!isContractUid(uid)) {
    print_("# a UID is 8 to 20 hex characters, e.g. 04A1B2C3\r\n");
    return;
  }
  tapCard(uid);
}

void NodeConsole::allow(char* uid, const char* roleName) {
  char text[96];
  aeroion_role_t role;
  upperCase(uid);
  if (!isContractUid(uid)) {
    print_("# a UID is 8 to 20 hex characters, e.g. 04A1B2C3\r\n");
  } else if (!badgeRoleFromName(roleName, &role)) {
    print_("# role must be operator, technician, admin, household or unknown\r\n");
  } else if (!badges_.set(uid, role)) {
    print_("# the badge list is full (64 badges)\r\n");
  } else {
    snprintf(text, sizeof text, "# %s is now %s on this node, until the station sends its list\r\n", uid,
             aeroion_role_name(role));
    print_(text);
  }
}

void NodeConsole::forget(char* uid) {
  char text[64];
  upperCase(uid);
  snprintf(text, sizeof text, badges_.remove(uid) ? "# %s removed from the badge list\r\n"
                                                  : "# %s is not on the badge list\r\n",
           uid);
  print_(text);
}

void NodeConsole::printBadges() {
  char text[96];
  if (badges_.fromStation()) {
    snprintf(text, sizeof text, "# badge list: %d badge(s), version %lu from the station\r\n", badges_.count(),
             static_cast<unsigned long>(badges_.version()));
  } else {
    snprintf(text, sizeof text, "# badge list: %d badge(s), from mcx_node_config.h\r\n", badges_.count());
  }
  print_(text);
  for (int i = 0; i < badges_.count(); ++i) {
    snprintf(text, sizeof text, "#   %-20s %s\r\n", badges_.uidAt(i), aeroion_role_name(badges_.roleAt(i)));
    print_(text);
  }
}

void NodeConsole::printState() {
  char text[192];
  const char* fault = safety_.blockingFault();
  snprintf(text, sizeof text, "# state %s (%s)%s%s\r\n", aeroion_state_name(safety_.state()), safety_.lastReason(),
           fault ? ", cannot arm: " : "", fault ? fault : "");
  print_(text);
}

void NodeConsole::printSensors(uint32_t nowMs) {
  char text[96];
  for (int i = 0; i < registry_.count(); ++i) {
    snprintf(text, sizeof text, "# sensor %-12s %-8s errors %lu\r\n", registry_.sensorAt(i)->name(),
             sensorStateName(registry_.stateAt(i, nowMs)), static_cast<unsigned long>(registry_.errorsAt(i)));
    print_(text);
  }
  sensors_.printRaw(print_);
}

// Asks every 7-bit address whether a device is there.
void NodeConsole::scanI2c() {
  char text[48];
  int found = 0;
  for (uint8_t address = 0x08; address <= 0x77; ++address) {
    if (io_i2c_write(address, nullptr, 0)) {
      snprintf(text, sizeof text, "# i2c device at 0x%02X\r\n", address);
      print_(text);
      found++;
    }
  }
  snprintf(text, sizeof text, "# i2c scan done: %d device(s)\r\n", found);
  print_(text);
}

void NodeConsole::zero() {
  if (safety_.hvAllowed()) {
    print_("# zero refused: the high voltage may be on (LOCK the node first)\r\n");
  } else if (sensors_.zeroCurrent()) {
    print_("# collector-current zero measured again\r\n");
    sensors_.printRaw(print_);
  } else {
    print_("# zero failed: no collector-current sensor, or the ADC did not answer\r\n");
  }
}