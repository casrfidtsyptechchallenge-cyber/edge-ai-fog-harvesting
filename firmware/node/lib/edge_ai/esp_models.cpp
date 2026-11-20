#include "esp_models.h"

#include <math.h>
#include <string.h>

namespace {

double roundTo(float value, int decimals) {
  double scale = pow(10.0, decimals);
  return round(static_cast<double>(value) * scale) / scale;
}

// A number field of the MCX's telemetry, or NaN when it is null or missing.
float numberOf(JsonObjectConst payload, const char* key) {
  JsonVariantConst v = payload[key];
  return v.is<float>() ? v.as<float>() : NAN;
}

}  // namespace

void EspModels::telemetry(JsonObject payload, uint32_t nowMs) {
  readings_++;
  bool hvOn = payload["hv_on"].is<bool>() && payload["hv_on"].as<bool>();
  float level = numberOf(payload, "hv_level");
  float current = numberOf(payload, "i_ua");
  float fogOpt = numberOf(payload, "fog_idx_opt");

  // Once the virtual fog sensor knows how much fog lowers the current, the health fit gets the current as it
  // would be in clear air: a fog thickening during a sweep would otherwise tilt the fit and move the onset.
  float forFit = current;
  if (fog_.calibrated() && isfinite(fogOpt) && isfinite(current)) {
    float factor = 1.0f - fog_.suppression() * fogOpt;
    if (factor > 0.2f) forFit = current / factor;
  }
  health_.add(level, forFit, hvOn, nowMs);
  lastFog_ = fog_.update(level, current, hvOn, health_.onset(),
                         isfinite(fogOpt) ? Nullable<float>(fogOpt) : Nullable<float>());
  if (lastFog_.has()) payload["fog_idx_virt"] = roundTo(lastFog_.get(), 3);

  JsonObject health = payload["health"];
  if (health.isNull()) health = payload["health"].to<JsonObject>();
  Nullable<float> onset = health_.onset(), k = health_.kCoef(), score = health_.score();
  if (onset.has()) health["onset_level"] = roundTo(onset.get(), 4);
  if (k.has()) health["k_coef"] = roundTo(k.get(), 1);
  if (score.has()) health["score"] = roundTo(score.get(), 3);
  static const char* const kKeys[] = {"onset_level", "k_coef", "score"};
  for (const char* key : kKeys) {
    if (health[key].isNull()) health[key] = nullptr;  // the contract wants all three, null when unknown
  }
}

void EspModels::event(JsonObjectConst payload, uint32_t nowMs) {
  (void)nowMs;
  const char* type = payload["type"] | "";
  if (strcmp(type, "MAINT_OFF") == 0) clean();
}

void EspModels::clean() {
  health_.setReference(NAN);
  referenceCleared_ = true;
}

bool EspModels::takeReferenceCleared() {
  bool cleared = referenceCleared_;
  referenceCleared_ = false;
  return cleared;
}