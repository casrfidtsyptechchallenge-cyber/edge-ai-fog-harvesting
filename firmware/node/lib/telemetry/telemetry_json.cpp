#include "telemetry_json.h"

#include <math.h>

namespace {

double roundTo(float value, int decimals) {
  double scale = pow(10.0, decimals);
  return round(static_cast<double>(value) * scale) / scale;
}

// A missing value, or a NaN/infinity from a faulty computation, becomes null: JSON has no NaN.
void putNumber(JsonObject out, const char* key, const Nullable<float>& value, int decimals) {
  if (value.has() && isfinite(value.get())) {
    out[key] = roundTo(value.get(), decimals);
  } else {
    out[key] = nullptr;
  }
}

void putBool(JsonObject out, const char* key, const Nullable<bool>& value) {
  if (value.has()) {
    out[key] = value.get();
  } else {
    out[key] = nullptr;
  }
}

// Takes `const char*` on purpose: given a char ARRAY such as NodeEvent::text, ArduinoJson keeps only a
// pointer to it (it treats arrays like string literals), and the event may be gone before the message is
// written. A `const char*` is copied into the document.
void putText(JsonObject out, const char* key, const char* text) { out[key] = text; }

void putHeader(JsonObject out, const NodeIdentity& id, uint64_t tsMs) {
  out["schema_version"] = AEROION_SCHEMA_VERSION;
  out["node_id"] = id.nodeId;
  out["ts"] = tsMs;
}

}  // namespace

void buildTelemetryPayload(JsonObject out, const NodeIdentity& id, uint64_t tsMs, uint32_t seq,
                           const NodeTelemetry& t) {
  const SensorFrame& s = t.sensors;
  putHeader(out, id, tsMs);
  out["seq"] = seq;
  out["state"] = aeroion_state_name(t.state);
  out["hv_on"] = t.hvOn;
  out["hv_level"] = roundTo(t.hvLevel, 3);
  putNumber(out, "fog_idx_opt", s.fogIdxOpt, 3);
  putNumber(out, "fog_idx_virt", t.fogIdxVirt, 3);
  putNumber(out, "t_c", s.tempC, 2);
  putNumber(out, "rh_pct", s.rhPct, 1);
  putNumber(out, "mass_g", s.massG, 2);
  putNumber(out, "rate_gpm", s.rateGpm, 3);
  putNumber(out, "i_ua", s.currentUa, 2);
  putNumber(out, "p_w", s.powerW, 3);
  putNumber(out, "e_wh", s.energyWh, 4);
  putNumber(out, "arc_risk", t.arcRisk, 3);
  if (t.arcNovel.has()) putNumber(out, "arc_novel", t.arcNovel, 2);  // optional field (contract 1.3)
  JsonObject health = out["health"].to<JsonObject>();
  putNumber(health, "onset_level", t.onsetLevel, 4);
  putNumber(health, "k_coef", t.kCoef, 1);
  putNumber(health, "score", t.healthScore, 3);
  putBool(out, "radar_presence", s.radarPresence);
  putBool(out, "lid_closed", s.lidClosed);
  putNumber(out, "batt_pct", s.battPct, 1);
}

void buildStatusPayload(JsonObject out, const NodeIdentity& id, uint64_t tsMs, uint32_t uptimeS,
                        bool timeSynced) {
  out["schema_version"] = AEROION_SCHEMA_VERSION;
  out["node_id"] = id.nodeId;
  out["online"] = true;
  out["ts"] = tsMs;
  out["uptime_s"] = uptimeS;
  out["time_synced"] = timeSynced;
  out["buffered_msgs"] = 0;
}

void buildInfoPayload(JsonObject out, const NodeIdentity& id, uint64_t tsMs, const char* const* sensorNames,
                      int sensorCount) {
  putHeader(out, id, tsMs);
  out["fw"] = id.fwVersion;
  JsonObject models = out["models"].to<JsonObject>();  // no on-device models yet (M5)
  models["arc"] = nullptr;
  models["fog"] = nullptr;
  models["health"] = nullptr;
  JsonObject position = out["position"].to<JsonObject>();
  position["x_m"] = roundTo(id.xM, 2);
  position["y_m"] = roundTo(id.yM, 2);
  JsonArray sensors = out["sensors"].to<JsonArray>();
  for (int i = 0; i < sensorCount; ++i) sensors.add(sensorNames[i]);
}

void buildEventPayload(JsonObject out, const NodeIdentity& id, uint64_t tsMs, uint32_t seq, const NodeEvent& e) {
  putHeader(out, id, tsMs);
  out["seq"] = seq;
  out["type"] = aeroion_event_name(e.type);
  JsonObject data = out["data"].to<JsonObject>();
  switch (e.type) {
    case AEROION_EVENT_FOG_DETECTED:
      data["fog_idx"] = roundTo(e.fogIdx, 3);
      data["source"] = aeroion_fog_source_name(e.fogSource);
      break;
    case AEROION_EVENT_FOG_CLEARED:
      data["duration_s"] = roundTo(e.durationS, 1);
      break;
    case AEROION_EVENT_ARC_PREDICTED:
      data["risk"] = roundTo(e.risk, 3);
      data["hv_level"] = roundTo(e.hvLevel, 3);
      break;
    case AEROION_EVENT_ARC:
      data["peak_ua"] = roundTo(e.peakUa, 1);
      data["hv_level"] = roundTo(e.hvLevel, 3);
      data["predicted"] = e.predicted;
      break;
    case AEROION_EVENT_STATE_CHANGE:
      data["from"] = aeroion_state_name(e.from);
      data["to"] = aeroion_state_name(e.to);
      putText(data, "reason", e.text);
      break;
    case AEROION_EVENT_BADGE_TAP:
      putText(data, "uid", e.id);
      data["role"] = aeroion_role_name(e.role);
      data["action"] = aeroion_badge_action_name(e.action);
      data["result"] = aeroion_badge_result_name(e.result);
      break;
    case AEROION_EVENT_FAULT:
      data["code"] = aeroion_fault_name(e.fault);
      putText(data, "detail", e.text);
      break;
    case AEROION_EVENT_MAINT_ON:
    case AEROION_EVENT_MAINT_OFF:
      putText(data, "uid", e.id);
      break;
    case AEROION_EVENT_CMD_ACK:
      putText(data, "cmd_id", e.id);
      data["result"] = aeroion_ack_name(e.ack);
      putText(data, "reason", e.text);
      break;
    default:
      break;
  }
}


size_t writeLine(char* out, size_t cap, const char* topic, bool retain, JsonObjectConst payload) {
  JsonDocument line;
  line["topic"] = topic;
  line["retain"] = retain;
  line["payload"] = payload;
  if (measureJson(line) + 1 > cap) return 0;
  return serializeJson(line, out, cap);
}