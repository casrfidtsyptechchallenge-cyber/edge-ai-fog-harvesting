#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

#include "electrode_health.h"
#include "node_gateway.h"
#include "virtual_fog.h"

// The ESP32's models (architecture v2): the electrode health (M5c) and the virtual fog sensor (M5b), run on
// the MCX's telemetry as it passes through the gateway (once a second). They fill fog_idx_virt and health,
// which the MCX leaves null. The arc predictor (M5a) needs the fast current windows: it runs on the MCX.
// They help each other: the health gives the virtual fog its onset V0; once calibrated, the virtual fog's
// suppression turns the current into clear-air current for the health fit (and k_coef is then clear-air K).
//
// The end of a maintenance lock-out (MAINT_OFF, the technician's badge) means the electrode was serviced:
// the health learns its clean reference again. So does the ESP32's console command "clean".
class EspModels : public TelemetryHook {
 public:
  explicit EspModels(const HealthConfig& health = HealthConfig(), const VirtualFogConfig& fog = VirtualFogConfig())
      : health_(health), fog_(fog), lastFog_(), readings_(0), referenceCleared_(false) {}

  void telemetry(JsonObject payload, uint32_t nowMs) override;
  void event(JsonObjectConst payload, uint32_t nowMs) override;

  void clean();  // the electrode was cleaned or replaced: learn the reference again
  // True once after clean() or MAINT_OFF: the ESP32 then forgets the reference it saved.
  bool takeReferenceCleared();

  ElectrodeHealth& health() { return health_; }
  VirtualFog& fog() { return fog_; }
  Nullable<float> lastFog() const { return lastFog_; }
  uint32_t readings() const { return readings_; }

 private:
  ElectrodeHealth health_;
  VirtualFog fog_;
  Nullable<float> lastFog_;
  uint32_t readings_;
  bool referenceCleared_;
};