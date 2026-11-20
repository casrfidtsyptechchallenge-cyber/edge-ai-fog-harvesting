#pragma once
#include <stddef.h>
#include <stdint.h>

#include "arc_predictor.h"
#include "nullable.h"

// Where the windows of collector current come from: the MCX's fast ADC (firmware/mcx_node), a fake in tests.
class ArcWindowSource {
 public:
  virtual ~ArcWindowSource() {}
  // Fills ua[0..n-1] with the collector current in µA, one sample every 1/fsHz s as nearly as it can.
  // Returns the sample rate it really achieved (Hz), or 0 if it failed.
  virtual float capture(float* ua, int n, float fsHz) = 0;
};

// The arc predictor at work (M5a, on the MCX): while the HV is allowed, a window every periodMs through the
// predictor; its risk goes to the safety core (SafetyInputs.arcRisk), which backs off above its threshold.
// The last window is kept, to be sent as a "wave" message (training data, M2 windows.csv) on demand.
class ArcGuard {
 public:
  typedef uint32_t (*MicrosFn)();  // a microsecond clock, to measure the time taken (may be null)

  ArcGuard(ArcWindowSource& source, uint32_t periodMs = 100, MicrosFn micros = nullptr);

  // Call often (every safety cycle). Returns true when a new window was taken and judged.
  bool poll(bool hvOn, uint32_t nowMs);

  // The latest risk; null while the HV is off, before the first window, or when a capture fails
  // (contract: arc_risk is null when the model is not running).
  Nullable<float> risk() const { return risk_; }
  ArcPredictor& predictor() { return predictor_; }
  const ArcWindowInfo& info() const { return predictor_.last(); }

  const float* window() const { return window_; }
  int length() const { return length_; }
  float fsHz() const { return fsHz_; }
  uint32_t windowMs() const { return windowMs_; }  // when the last window was taken (poll's nowMs)
  uint32_t windows() const { return windows_; }
  uint32_t failures() const { return failures_; }
  uint32_t captureUs() const { return captureUs_; }    // 0 without a microsecond clock
  uint32_t inferenceUs() const { return inferenceUs_; }

  // Novelty guard (arc_novelty.h): share of the last kNoveltySpan windows unlike the training windows.
  // Null while the HV is off or before the first window (telemetry arc_novel). The risk is not changed by it.
  static const int kNoveltySpan = 50;  // 5 s at one window every 100 ms
  Nullable<float> novelShare() const;
  bool lastNovel() const { return lastNovel_; }
  uint32_t novelWindows() const { return novelWindows_; }

 private:
  ArcWindowSource& source_;
  ArcPredictor predictor_;
  uint32_t periodMs_;
  MicrosFn micros_;
  float window_[ArcPredictor::kWindow];
  int length_;
  float fsHz_;
  bool started_;
  uint32_t lastMs_;
  uint32_t windowMs_;
  uint32_t windows_;
  uint32_t failures_;
  uint32_t captureUs_;
  uint32_t inferenceUs_;
  Nullable<float> risk_;
  uint8_t novel_[kNoveltySpan];  // 1 = that window was novel (ring)
  int novelCount_;
  int novelNext_;
  bool lastNovel_;
  uint32_t novelWindows_;
};

// One "wave" message (contract wave.schema.json) as a JSON line for the link, written without a JSON library
// (256 numbers do not fit the MCX's small heap). Samples in µA with one decimal. Returns the length written,
// or 0 if it does not fit in cap.
size_t formatWaveLine(char* out, size_t cap, const char* nodeId, const char* schemaVersion, uint64_t ts,
                      uint32_t seq, const char* trigger, int fsHz, const float* ua, int n);