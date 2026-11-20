#pragma once
#include <stdint.h>

#include "aeroion_contract.h"
#include "nullable.h"
#include "sensor_frame.h"

// Who this node is. Filled from node_config.h.
struct NodeIdentity {
  const char* nodeId;
  const char* fwVersion;
  float xM;
  float yM;
};

// Everything a telemetry message carries. Each module fills its own part.
// In M1 only `sensors` is real; the other parts keep these safe defaults until their module exists.
struct NodeTelemetry {
  aeroion_state_t state;        // M3 safety core
  bool hvOn;                    // M3
  float hvLevel;                // M3 / M6, 0..1
  SensorFrame sensors;          // M1 sensing
  Nullable<float> fogIdxVirt;   // M5b virtual fog sensor
  Nullable<float> arcRisk;      // M5a arc predictor
  Nullable<float> arcNovel;     // M5a novelty guard: share of recent windows unlike the training data
  Nullable<float> onsetLevel;   // M5c electrode health
  Nullable<float> kCoef;        // M5c
  Nullable<float> healthScore;  // M5c

  NodeTelemetry() : state(AEROION_STATE_LOCKED), hvOn(false), hvLevel(0.0f) {}
};