#pragma once
#include <stdint.h>

#include "node_event.h"
#include "nullable.h"
#include "safety_config.h"

// Turns the fog index into "fog present" with hysteresis, so it does not flicker, and sends
// FOG_DETECTED / FOG_CLEARED. No reading counts as no fog: without evidence of fog the HV stays off.
class FogDetector {
 public:
  FogDetector(const SafetyConfig& config, EventSink& events);

  void update(Nullable<float> fogIdx, aeroion_fog_source_t source, bool prearmed, uint32_t nowMs);
  bool present() const { return present_; }

 private:
  SafetyConfig config_;
  EventSink& events_;
  bool present_;
  uint32_t sinceMs_;
};