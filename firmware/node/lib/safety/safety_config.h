#pragma once
#include <stdint.h>

// Limits and timings of the safety core (M3). The defaults match the simulator (sim/fake_node.py).
// Values that depend on the hardware are set in main.cpp from node_config.h.
struct SafetyConfig {
  // Fog detection, with hysteresis: fog starts above fogOn and ends below fogOff.
  float fogOn = 0.30f;
  float fogOnPrearmed = 0.20f;    // lower threshold after a PREARM from the station
  float fogOff = 0.12f;
  uint32_t prearmExtraMs = 60000;  // a PREARM lasts eta_s plus this

  // High-voltage level, as a fraction 0..1 of the HV module's range.
  float levelMin = 0.30f;    // below this there is no corona, so no point being on
  float levelMax = 0.95f;    // hard ceiling, whatever the controller asks for
  float rampUpPerS = 0.05f;  // the level rises slowly; it drops at once

  // Arcs.
  float arcWarnRisk = 0.70f;          // arc_risk (M5a) above this: ARC_PREDICTED and BACKOFF
  uint32_t backoffMs = 4000;          // how long BACKOFF lasts
  float backoffDrop = 0.12f;          // how much the level drops when backing off
  float arcCurrentUa = 250.0f;        // collector current above this is an arc
  uint32_t overcurrentMs = 1000;      // above it for this long: HV_OVERCURRENT fault
  uint8_t maxArcs = 3;                // this many arcs...
  uint32_t arcWindowMs = 60000;       // ...within this time: HV_OVERCURRENT fault
  uint32_t predictedWindowMs = 5000;  // an arc this soon after ARC_PREDICTED counts as predicted

  // Interlock sensors. When required, a sensor that stops reporting is a fault: the node cannot run
  // without seeing people or the lid, nor with the HV on and no way to see an arc.
  bool requireRadar = true;
  bool requireLid = true;
  bool requireCurrent = true;  // collector current: without it, no arc detection and no arc predictor

  // Plausibility of the collector current. A sensor whose wire came off does not stop reporting: it reads
  // about 0 µA. With the HV really on at a level well above the corona onset, that cannot be true, so after
  // lowCurrentMs it is a SENSOR_FAIL fault. Off (0) until the node has a real HV output: on the bench without
  // one, "HV allowed" comes with no current at all. mcx_node_config.h turns it on (AEROION_HAS_HV_OUTPUT).
  float minCurrentUa = 0.0f;          // 0: check off
  float minCurrentFromLevel = 0.45f;  // only at or above this level (well above the corona onset, ~0.25)
  uint32_t lowCurrentMs = 3000;       // the HV module and its output capacitor need time to come up

  // People. After the radar saw someone, the area must stay clear this long before the node re-arms by
  // itself: a presence radar can lose a person who stands still for a moment.
  uint32_t presenceClearMs = 10000;
};