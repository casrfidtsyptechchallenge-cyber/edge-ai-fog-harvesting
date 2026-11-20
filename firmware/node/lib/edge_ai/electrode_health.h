#pragma once
#include <stdint.h>

#include "nullable.h"

// Electrode health (M5c): the Townsend law of corona, I = K·V·(V − V0), fitted on the node's own readings.
//
// V is the HV level (0..1), I the collector current (µA). Written as I/V = K·V − K·V0 it is a straight line,
// fitted by least squares on the readings of the last minute once the level has moved enough (the
// controller's steps, a ramp up, a back-off). Fog lowers the current by a factor (that is the virtual fog
// sensor, M5b), which changes K but not V0 = −intercept/slope: the onset is measured whatever the fog.
//
// A dirty or worn electrode needs more voltage to start the corona: the onset rises. The score compares the
// onset with the clean electrode's (the reference): 1 = like new, 0 = risen by driftFull (clean it).
// Plain logic with no hardware: tested on the PC (test/test_edge_ai).
struct HealthConfig {
  uint32_t windowMs = 60000;   // readings of the last minute go into one fit (the fog changes slowly)
  float minSpan = 0.10f;       // the level must have moved this much within the window
  int minPoints = 8;           // readings on the Townsend curve (current above minCurrentUa)
  float minCurrentUa = 1.0f;   // below: no corona yet, not on the curve
  float minR2 = 0.90f;         // a fit explaining less of the readings is not used
  float onsetAlpha = 0.2f;     // each accepted fit moves the onset estimate this far
  float driftFull = 0.15f;     // onset risen this much above the reference: score 0
  int referenceFits = 3;       // without a reference set, the mean of the first fits becomes it
};

class ElectrodeHealth {
 public:
  static const int kMaxPoints = 64;

  explicit ElectrodeHealth(const HealthConfig& config = HealthConfig());

  // One reading (once a second): HV level 0..1, collector current µA, HV on. Returns true when a new fit
  // was accepted.
  bool add(float level, float currentUa, bool hvOn, uint32_t nowMs);

  // The clean electrode's onset (after cleaning, or from the commissioning). NaN or <= 0: learn it again.
  void setReference(float onset);
  Nullable<float> reference() const;

  Nullable<float> onset() const;     // estimated V0 (hv_level units)
  Nullable<float> kCoef() const;     // K of the last fit (µA per level²; lowered by fog unless corrected for it)
  Nullable<float> score() const;     // 1 like new .. 0 clean it; null until there is a reference
  int fits() const { return fits_; }
  float lastR2() const { return lastR2_; }

 private:
  struct Point {
    float level;
    float currentUa;
    uint32_t ms;
  };
  bool fit(float* k, float* v0, float* r2) const;
  void drop(uint32_t nowMs);

  HealthConfig config_;
  Point points_[kMaxPoints];
  int count_;
  int fits_;
  float onset_;
  float k_;
  float lastR2_;
  float reference_;
  bool referenceSet_;
  float firstOnsets_;   // sum of the first fits' onsets, for the learned reference
};