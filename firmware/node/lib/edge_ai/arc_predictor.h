#pragma once
#include <stdint.h>

// The arc predictor (M5a): a window of collector current in, the probability of an arc within 5 s out.
//
//   kAuto   the 8-bit CNN of arc_model_data.h when the window is what it was trained on (256 samples at
//           10 kHz, within 10 %); else the rule-based fallback
//   kModel  the CNN only (a window it cannot take gives no risk)
//   kRules  the rule-based fallback only
//
// Both start from the same preparation of the window (sorted copy: median and noise; pulses above the noise),
// exactly as ml/arc_cnn.py does: the PC tests check the CNN's integers against the Python reference.
// Trained on synthetic windows for now (ml/arc_physics.py): retrain on chamber data (M11).
struct ArcWindowInfo {
  float medianUa;     // the window's median current
  float noise;        // noise sigma, relative to max(median, 2 µA)
  float pulsesPerS;   // pre-discharge pulses per second
  float peakUa;       // the highest sample: an arc shows here even when it is too short for the 10 Hz reading
  float modelRisk;    // -1: the CNN did not run
  float ruleRisk;
  bool usedModel;     // the risk given out is the CNN's
  uint8_t novelty;    // ArcNovelty::Reason: 0 known, 1 saturated, 2 out of range, 3 unusual shape
  float noveltyD2;    // squared Mahalanobis distance of the CNN's features (-1: the CNN did not run)
};

class ArcPredictor {
 public:
  enum Mode { kAuto, kModel, kRules };
  static const int kWindow = 256;
  static const int kFeatures = 32;

  ArcPredictor() : mode_(kAuto), last_() {}

  void setMode(Mode mode) { mode_ = mode; }
  Mode mode() const { return mode_; }
  static const char* modeName(Mode mode);
  static const char* modelName();
  static float modelSampleRateHz();  // the rate the CNN was trained at (10 kHz)

  // n samples in µA (16 to 256), taken at fsHz. Returns the risk 0..1, or -1 if nothing could be computed.
  float predict(const float* ua, int n, float fsHz);
  const ArcWindowInfo& last() const { return last_; }

  // The steps, public for the tests.
  // Sorts, then fills info (median, noise, pulses, peak) and, when input is not null and n is 256, the CNN's
  // int8 input.
  static void prepare(const float* ua, int n, float fsHz, int8_t* input, ArcWindowInfo* info);
  // The int8 network up to the dense layer's sum (features: the 32 pooled values, may be null).
  static int32_t networkSum(const int8_t* input, int8_t* features);
  static float modelRisk(int32_t sum);
  static float ruleRisk(float pulsesPerS, float noise, float medianUa);

 private:
  Mode mode_;
  ArcWindowInfo last_;
  int8_t input_[kWindow];
};