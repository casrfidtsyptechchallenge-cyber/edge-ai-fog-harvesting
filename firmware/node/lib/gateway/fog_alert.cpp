#include "fog_alert.h"

#include <string.h>

bool packFogAlert(const FogAlert& alert, uint8_t out[kFogAlertSize]) {
  if (alert.nodeNum < 1 || alert.nodeNum > 99) return false;
  float fog = alert.fogIdx;
  if (!(fog >= 0.0f)) fog = 0.0f;  // also a NaN
  if (fog > 1.0f) fog = 1.0f;
  out[0] = AEROION_FOG_ALERT_MAGIC;
  out[1] = AEROION_FOG_ALERT_VERSION;
  out[2] = AEROION_FOG_ALERT_TYPE;
  out[3] = static_cast<uint8_t>(alert.nodeNum);
  for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<uint8_t>(alert.tsMs >> (8 * i));  // little-endian
  out[12] = static_cast<uint8_t>(alert.seq & 0xFF);
  out[13] = static_cast<uint8_t>(alert.seq >> 8);
  out[14] = static_cast<uint8_t>(fog * 255.0f + 0.5f);
  out[15] = (alert.timeSynced ? AEROION_FOG_ALERT_FLAG_TIME_SYNCED : 0) |
            (alert.virtualSource ? AEROION_FOG_ALERT_FLAG_VIRTUAL_SOURCE : 0);
  return true;
}

FogAlertRead readFogAlert(const uint8_t* data, size_t length, FogAlert* out) {
  if (data == nullptr || length != kFogAlertSize) return kAlertWrongSize;
  if (data[0] != AEROION_FOG_ALERT_MAGIC) return kAlertForeign;
  if (data[1] != AEROION_FOG_ALERT_VERSION || data[2] != AEROION_FOG_ALERT_TYPE) return kAlertUnsupported;
  if (data[3] < 1 || data[3] > 99) return kAlertBadNode;
  out->nodeNum = data[3];
  out->tsMs = 0;
  for (int i = 7; i >= 0; --i) out->tsMs = (out->tsMs << 8) | data[4 + i];
  out->seq = static_cast<uint16_t>(data[12] | (data[13] << 8));
  out->fogIdx = data[14] / 255.0f;
  out->timeSynced = (data[15] & AEROION_FOG_ALERT_FLAG_TIME_SYNCED) != 0;
  out->virtualSource = (data[15] & AEROION_FOG_ALERT_FLAG_VIRTUAL_SOURCE) != 0;
  return kAlertOk;
}

int nodeNumberOf(const char* nodeId) {
  if (nodeId == nullptr || strlen(nodeId) != 3 || nodeId[0] != 'n') return -1;
  if (nodeId[1] < '0' || nodeId[1] > '9' || nodeId[2] < '0' || nodeId[2] > '9') return -1;
  int number = (nodeId[1] - '0') * 10 + (nodeId[2] - '0');
  return number >= 1 ? number : -1;
}

FogAlertFilter::FogAlertFilter() {
  memset(heard_, 0, sizeof heard_);
  memset(seq_, 0, sizeof seq_);
  memset(tsMs_, 0, sizeof tsMs_);
}

bool FogAlertFilter::isNew(const FogAlert& alert) {
  int n = alert.nodeNum;
  if (n < 1 || n > 99) return false;
  if (heard_[n] && seq_[n] == alert.seq && tsMs_[n] == alert.tsMs) return false;
  heard_[n] = true;
  seq_[n] = alert.seq;
  tsMs_[n] = alert.tsMs;
  return true;
}