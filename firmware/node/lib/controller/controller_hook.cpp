#include "controller_hook.h"

#include <math.h>
#include <string.h>

namespace {

Nullable<float> numberOf(JsonVariantConst v) {
  if (!v.is<float>()) return Nullable<float>();
  float x = v.as<float>();
  return isfinite(x) ? Nullable<float>(x) : Nullable<float>();
}

}  // namespace

void ControllerHook::telemetry(JsonObject payload, uint32_t nowMs) {
  ControllerInput in;
  const char* state = payload["state"] | "";
  in.harvest = strcmp(state, "HARVEST") == 0;
  in.hvOn = payload["hv_on"] | false;
  in.level = payload["hv_level"] | 0.0f;
  in.massG = numberOf(payload["mass_g"]);
  in.rateGpm = numberOf(payload["rate_gpm"]);
  in.powerW = numberOf(payload["p_w"]);
  in.arcRisk = numberOf(payload["arc_risk"]);
  in.arcNovel = numberOf(payload["arc_novel"]);  // absent before contract 1.3: null
  in.fog = numberOf(payload["fog_idx_opt"]);
  if (!in.fog.has()) in.fog = numberOf(payload["fog_idx_virt"]);  // the optical sensor is out: M5b
  in.onset = numberOf(payload["health"]["onset_level"]);
  controller_.telemetry(in, nowMs);
}

void ControllerHook::event(JsonObjectConst payload, uint32_t nowMs) {
  const char* type = payload["type"] | "";
  JsonVariantConst data = payload["data"];
  if (strcmp(type, "ARC") == 0 || strcmp(type, "ARC_PREDICTED") == 0) {
    controller_.arc(data["hv_level"] | NAN, nowMs);
  } else if (strcmp(type, "CMD_ACK") == 0 && strncmp(data["cmd_id"] | "", "m6-", 3) == 0) {
    if (strcmp(data["result"] | "", "accepted") == 0) {
      accepted_++;
    } else {
      rejected_++;
    }
  }
}

void ControllerHook::command(JsonObjectConst cmd, uint64_t unixMs, uint32_t nowMs) {
  if (strcmp(cmd["type"] | "", "SET_HINT") != 0) return;
  uint32_t ttlS = cmd["ttl_s"] | 0U;
  uint64_t validMs = static_cast<uint64_t>(ttlS) * 1000ULL;
  if (cmd["ts"].is<uint64_t>()) {  // valid until ts + ttl_s, by the ESP32's clock
    uint64_t until = cmd["ts"].as<uint64_t>() + validMs;
    uint64_t left = until > unixMs ? until - unixMs : 0;
    if (left < validMs) validMs = left;
  }
  controller_.stationHint(static_cast<uint32_t>(validMs), nowMs);
}

bool ControllerHook::levelRequest(float* level, uint32_t* ttlS, uint32_t nowMs) {
  if (!controller_.takeRequest(level, nowMs)) return false;
  *ttlS = controller_.config().ttlS;
  return true;
}