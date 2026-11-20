#pragma once
#include <stddef.h>
#include <stdint.h>

#include "aeroion_contract.h"

// FOG_ALERT (contract/binary/fog_alert.json): the 16-byte radio message a node broadcasts to its neighbours
// (ESP-NOW) when it detects fog, so they get ready even while the station is down. Packed little-endian; the
// bytes are written one by one, so they are the same on every computer (the PC tests, the ESP32, Python).
struct FogAlert {
  int nodeNum;         // 1 to 99: node n07 sends 7
  uint64_t tsMs;       // detection time, Unix ms
  uint16_t seq;        // the sender's alert counter
  float fogIdx;        // 0..1, sent as fog_q = fog x 255
  bool timeSynced;     // the sender's clock was synced
  bool virtualSource;  // detected by the virtual fog sensor (M5b)
};

const size_t kFogAlertSize = sizeof(aeroion_fog_alert_t);  // 16

// Writes the 16 bytes. False if the node number is not 1 to 99.
bool packFogAlert(const FogAlert& alert, uint8_t out[kFogAlertSize]);

enum FogAlertRead {
  kAlertOk,
  kAlertWrongSize,    // not 16 bytes: some other ESP-NOW traffic
  kAlertForeign,      // not our magic byte: some other ESP-NOW traffic
  kAlertUnsupported,  // our magic, but another version or type
  kAlertBadNode,      // node number not 1 to 99
};
FogAlertRead readFogAlert(const uint8_t* data, size_t length, FogAlert* out);

// "n07" -> 7; -1 if it is not a node id.
int nodeNumberOf(const char* nodeId);

// Each alert is sent several times (radio broadcasts have no acknowledgement). Remembers the last alert
// heard from every node, so each one counts once. Seq and time together: a node that restarts counts from 1
// again, but with a new time.
class FogAlertFilter {
 public:
  FogAlertFilter();
  bool isNew(const FogAlert& alert);  // false for a copy already heard

 private:
  bool heard_[100];
  uint16_t seq_[100];
  uint64_t tsMs_[100];
};