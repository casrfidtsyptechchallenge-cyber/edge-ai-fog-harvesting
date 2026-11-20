#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

#include "aeroion_contract.h"
#include "badge_list.h"
#include "node_event.h"
#include "safety_core.h"

// The station's commands (contract: cmd.schema.json) on the safety controller. Checks each command, hands
// it to the module it is for, and always answers with one CMD_ACK event:
//   LOCK            -> SafetyCore::lock (remote emergency stop)
//   PREARM          -> SafetyCore::prearm (fog is coming)
//   SET_HINT        -> the HV level the node asks the safety core for, until the command expires
//   SYNC_ALLOWLIST  -> the badge list (M4)
//   TARE            -> rejected: the load cell is on the node's ESP32, which answers TARE itself
// A command older than ts + ttl_s is answered "expired" and ignored, once the node's clock is synced; before
// that the node cannot tell its age and runs it. Running a command twice does no harm (MQTT may resend one).
class CommandHandler {
 public:
  CommandHandler(SafetyCore& safety, BadgeList& badges, EventSink& events);

  // One command (the payload of an aeroion/<node>/cmd or aeroion/all/cmd message). unixMs and timeSynced
  // come from the node's clock, nowMs is the time since boot. Returns the answer it sent. A command without
  // a usable cmd_id cannot be answered: it is ignored and the result is REJECTED with ackSent() false.
  aeroion_ack_t handle(JsonObjectConst cmd, uint64_t unixMs, bool timeSynced, uint32_t nowMs);

  // The HV level to ask the safety core for: the station's SET_HINT while it is valid, otherwise `fallback`.
  float requestedLevel(float fallback, uint32_t nowMs) const;
  bool hintActive(uint32_t nowMs) const;

  // The last command, for the console: its type, the answer and why.
  bool ackSent() const { return ackSent_; }  // false when it had no usable cmd_id
  const char* lastType() const { return lastType_; }
  aeroion_ack_t lastResult() const { return lastResult_; }
  const char* lastReason() const { return lastReason_; }
  uint32_t count(aeroion_ack_t result) const;  // answers sent since boot, by result

 private:
  SafetyReply run(aeroion_cmd_t type, JsonObjectConst params, uint32_t ageMs, uint32_t validMs, uint32_t nowMs);
  aeroion_ack_t answer(const char* cmdId, aeroion_ack_t result, const char* reason);

  SafetyCore& safety_;
  BadgeList& badges_;
  EventSink& events_;
  bool hint_;
  float hintLevel_;
  uint32_t hintStartMs_;
  uint32_t hintValidMs_;
  bool ackSent_;
  aeroion_ack_t lastResult_;
  char lastType_[16];
  char lastReason_[65];
  uint32_t counts_[3];
};

// Command type from its contract name ("LOCK", ...). False if there is no such type.
bool commandTypeFromName(const char* name, aeroion_cmd_t* type);