#pragma once
#include <stddef.h>
#include <stdint.h>

#include "badge_list.h"
#include "node_link.h"
#include "node_sensors.h"
#include "safety_core.h"
#include "sensor_registry.h"

// Commands typed in a serial terminal on the PC (type help), and every badge tap: cards read by the RFID
// reader, and the board's buttons as test badges (SW2 = operator, SW3 = technician). A tap looks up the
// card's role in the badge list; a card that is not on the list is "unknown" and the safety core denies it.
class NodeConsole {
 public:
  typedef void (*PrintFn)(const char* text);

  NodeConsole(SafetyCore& safety, SensorRegistry& registry, NodeSensors& sensors, BadgeList& badges,
              NodeLink& link, PrintFn print);
  void run(char* line, uint32_t nowMs);      // one typed line (main.cpp sends JSON lines to the link instead)
  void buttons(bool sw2Down, bool sw3Down);  // call every 10 ms with the button levels
  void tapCard(const char* uid);             // a card was presented (contract UID format)

 private:
  // A push button: press() is true once per press, after 3 equal samples in a row.
  class Button {
   public:
    Button() : raw_(false), stable_(false), count_(0) {}
    bool press(bool down);

   private:
    bool raw_;
    bool stable_;
    int count_;
  };

  void tapTyped(char* uid);
  void allow(char* uid, const char* roleName);
  void forget(char* uid);
  void printBadges();
  void printState();
  void printSensors(uint32_t nowMs);
  void scanI2c();
  void zero();

  SafetyCore& safety_;
  SensorRegistry& registry_;
  NodeSensors& sensors_;
  BadgeList& badges_;
  NodeLink& link_;
  PrintFn print_;
  Button sw2_;
  Button sw3_;
};