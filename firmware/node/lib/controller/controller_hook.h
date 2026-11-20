#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

#include "hv_controller.h"
#include "node_gateway.h"

// The HV controller (M6) on the ESP32's gateway: it reads the MCX's telemetry (after the ESP32's own
// readings and models are merged in: mass_g, rate_gpm, fog_idx_virt, health), the MCX's arc events, and the
// station's commands; the gateway sends its level requests to the MCX as SET_HINT (cmd_id "m6-<n>").
// A station SET_HINT passing through to the MCX takes over until it expires (e.g. tools/send_cmd.py hint).
class ControllerHook : public TelemetryHook {
 public:
  explicit ControllerHook(HvController& controller) : controller_(controller), accepted_(0), rejected_(0) {}

  void telemetry(JsonObject payload, uint32_t nowMs) override;
  void event(JsonObjectConst payload, uint32_t nowMs) override;
  void command(JsonObjectConst cmd, uint64_t unixMs, uint32_t nowMs) override;
  bool levelRequest(float* level, uint32_t* ttlS, uint32_t nowMs) override;

  uint32_t accepted() const { return accepted_; }  // the MCX's answers to the requests
  uint32_t rejected() const { return rejected_; }

 private:
  HvController& controller_;
  uint32_t accepted_;
  uint32_t rejected_;
};