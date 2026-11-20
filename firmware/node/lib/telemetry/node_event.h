#pragma once
#include <stddef.h>
#include <string.h>

#include "aeroion_contract.h"

// One event message (contract: event.schema.json). Each type uses only its own fields; create events
// with the functions below, e.g. events.emit(NodeEvent::fault(AEROION_FAULT_LID_OPEN, "lid open")).
struct NodeEvent {
  aeroion_event_t type;
  float fogIdx;                    // FOG_DETECTED
  aeroion_fog_source_t fogSource;  // FOG_DETECTED
  float durationS;                 // FOG_CLEARED
  float risk;                      // ARC_PREDICTED
  float hvLevel;                   // ARC_PREDICTED, ARC
  float peakUa;                    // ARC
  bool predicted;                  // ARC
  aeroion_state_t from;            // STATE_CHANGE
  aeroion_state_t to;              // STATE_CHANGE
  aeroion_fault_t fault;           // FAULT
  aeroion_role_t role;             // BADGE_TAP
  aeroion_badge_action_t action;   // BADGE_TAP
  aeroion_badge_result_t result;   // BADGE_TAP
  aeroion_ack_t ack;               // CMD_ACK
  char id[37];                     // uid (BADGE_TAP, MAINT_ON, MAINT_OFF) or cmd_id (CMD_ACK)
  char text[121];                  // reason (STATE_CHANGE, CMD_ACK: max 64) or detail (FAULT: max 120)

  static NodeEvent fogDetected(float fogIdx, aeroion_fog_source_t source) {
    NodeEvent e = blank(AEROION_EVENT_FOG_DETECTED);
    e.fogIdx = fogIdx;
    e.fogSource = source;
    return e;
  }
  static NodeEvent fogCleared(float durationS) {
    NodeEvent e = blank(AEROION_EVENT_FOG_CLEARED);
    e.durationS = durationS;
    return e;
  }
  static NodeEvent arcPredicted(float risk, float hvLevel) {
    NodeEvent e = blank(AEROION_EVENT_ARC_PREDICTED);
    e.risk = risk;
    e.hvLevel = hvLevel;
    return e;
  }
  static NodeEvent arc(float peakUa, float hvLevel, bool predicted) {
    NodeEvent e = blank(AEROION_EVENT_ARC);
    e.peakUa = peakUa;
    e.hvLevel = hvLevel;
    e.predicted = predicted;
    return e;
  }
  static NodeEvent stateChange(aeroion_state_t from, aeroion_state_t to, const char* reason) {
    NodeEvent e = blank(AEROION_EVENT_STATE_CHANGE);
    e.from = from;
    e.to = to;
    copyText(e.text, 65, reason);
    return e;
  }
  static NodeEvent badgeTap(const char* uid, aeroion_role_t role, aeroion_badge_action_t action,
                            aeroion_badge_result_t result) {
    NodeEvent e = blank(AEROION_EVENT_BADGE_TAP);
    copyText(e.id, 21, uid);
    e.role = role;
    e.action = action;
    e.result = result;
    return e;
  }
  static NodeEvent faultEvent(aeroion_fault_t code, const char* detail) {
    NodeEvent e = blank(AEROION_EVENT_FAULT);
    e.fault = code;
    copyText(e.text, 121, detail);
    return e;
  }
  static NodeEvent maintenance(bool on, const char* uid) {
    NodeEvent e = blank(on ? AEROION_EVENT_MAINT_ON : AEROION_EVENT_MAINT_OFF);
    copyText(e.id, 21, uid);
    return e;
  }
  static NodeEvent cmdAck(const char* cmdId, aeroion_ack_t ack, const char* reason) {
    NodeEvent e = blank(AEROION_EVENT_CMD_ACK);
    copyText(e.id, 37, cmdId);
    e.ack = ack;
    copyText(e.text, 65, reason);
    return e;
  }

  // Copies at most cap - 1 characters and always ends the text.
  static void copyText(char* out, size_t cap, const char* text) {
    strncpy(out, text ? text : "", cap - 1);
    out[cap - 1] = '\0';
  }

 private:
  static NodeEvent blank(aeroion_event_t type) {
    NodeEvent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    return e;
  }
};

// Where modules send their events. main.cpp prints them now (M1); M7 will publish them on MQTT.
class EventSink {
 public:
  virtual ~EventSink() {}
  virtual void emit(const NodeEvent& event) = 0;
};

// An RFID UID as the contract wants it: 8 to 20 upper-case hex characters (4 to 10 bytes).
inline bool isContractUid(const char* uid) {
  size_t n = uid ? strlen(uid) : 0;
  if (n < 8 || n > 20) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = uid[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}