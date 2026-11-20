#pragma once
#include <stdint.h>

#include "nullable.h"

// The virtual fog sensor (M5b): the collector is also the sensor. Fog droplets capture ions, so in fog the
// corona current is lower than the same voltage gives in clear air. With the onset V0 from the electrode
// health (M5c), each reading gives the momentary Townsend coefficient
//     k = I / (V·(V − V0))
// which falls as the fog thickens: k = K·(1 − s·fog). K (clear air) and s (how much fog lowers it) are
// learned from the optical sensor while it works (a least-squares line of k on fog_idx_opt, slowly
// forgetting old readings); then fog = (1 − k/K) / s comes from the current alone, and keeps coming when
// the optical sensor is fogged up, dirty or broken. They can also be set (from M11, after the chamber runs).
// Plain logic: tested on the PC (test/test_edge_ai).
struct VirtualFogConfig {
  float minAboveOnset = 0.05f;  // V must be this far above V0 (close to it, k is too noisy)
  float minCurrentUa = 1.0f;
  float forget = 0.9995f;       // each reading keeps this much of the past: about half an hour of memory
  float minWeight = 30.0f;      // readings (weighted) needed before the calibration is used
  float minFogSpread = 0.06f;   // the optical fog must have varied this much (standard deviation)
};

class VirtualFog {
 public:
  explicit VirtualFog(const VirtualFogConfig& config = VirtualFogConfig());

  // One reading (once a second). onset: V0 from ElectrodeHealth (null: no estimate). fogOpt: the optical
  // sensor (null while it is missing). Returns the virtual fog index 0..1, or null.
  Nullable<float> update(float level, float currentUa, bool hvOn, Nullable<float> onset, Nullable<float> fogOpt);

  void setCalibration(float kClear, float suppression);  // from the chamber runs; learning continues on top
  bool calibrated() const;
  float kClear() const { return kClear_; }
  float suppression() const { return suppression_; }

 private:
  void refit();

  VirtualFogConfig config_;
  double sw_, sf_, sk_, sff_, sfk_;   // weighted sums of the (fog_opt, k) pairs
  float kClear_;
  float suppression_;
  bool fitted_;
  bool fixed_;
};