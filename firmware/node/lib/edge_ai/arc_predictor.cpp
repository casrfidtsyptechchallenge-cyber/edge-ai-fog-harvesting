#include <algorithm>
#include "arc_predictor.h"
#include "arc_novelty.h"

#include <math.h>

#include "arc_model_data.h"

namespace {

const int kL1 = (ArcPredictor::kWindow - ARC_MODEL_K1) / 2 + 1;  // 125 steps after the first convolution
const int kL2 = (kL1 - ARC_MODEL_K2) / 2 + 1;                    // 61 after the second
const float kMinScaleUa = 2.0f;     // as ml/arc_cnn.py MIN_SCALE_UA
const int kNoiseIndex = 40;         // sorted[40] of 256: the 16th percentile
const double kPulseSigmas = 5.0;
const double kPulseMinZ = 0.05;
const float kFsTolerance = 0.10f;   // the CNN takes windows within 10 % of its sample rate

static_assert(ARC_MODEL_INPUT == ArcPredictor::kWindow, "arc_model_data.h is for another window length");
static_assert(2 * ARC_MODEL_C2 == ArcPredictor::kFeatures, "arc_model_data.h has another layer size");

// The scratch space of one inference: one at a time, so it can be shared (it does not fit on a small stack).
float gSorted[ArcPredictor::kWindow];
int8_t gLayer1[ARC_MODEL_C1][kL1];
int8_t gLayer2[ARC_MODEL_C2][kL2];

// int32 sum -> int8, with the integer multiplier and shift of ml/arc_cnn.py, then ReLU.
int8_t requantRelu(int32_t acc, int32_t m, int shift) {
  int64_t v = (static_cast<int64_t>(acc) * m + (static_cast<int64_t>(1) << (shift - 1))) >> shift;
  if (v > 127) v = 127;
  if (v < 0) v = 0;  // ReLU (and the clamp at -128 below it)
  return static_cast<int8_t>(v);
}

// The window's order statistics that prepare() needs: the two middle values (median), the 16th percentile
// (noise) and the maximum (peak). Selection instead of a full sort: the same values, exactly, for far fewer
// float comparisons (a full insertion sort of 256 floats cost about 1 M instructions without an FPU).
struct OrderStats {
  float lowMid, highMid, noiseQ, peak;
};

OrderStats orderStats(const float* x, int n, int noiseIndex, float* work) {
  for (int i = 0; i < n; ++i) work[i] = x[i];
  const int h = n / 2;
  std::nth_element(work, work + h, work + n);          // work[h]: the h-th smallest; before it: all smaller
  OrderStats s;
  s.highMid = work[h];
  s.lowMid = *std::max_element(work, work + h);        // the (h-1)-th smallest
  s.peak = *std::max_element(work + h, work + n);
  std::nth_element(work, work + noiseIndex, work + h);  // noiseIndex < h
  s.noiseQ = work[noiseIndex];
  return s;
}

}  // namespace

const char* ArcPredictor::modelName() { return ARC_MODEL_NAME; }
float ArcPredictor::modelSampleRateHz() { return ARC_MODEL_FS_HZ; }

const char* ArcPredictor::modeName(Mode mode) {
  switch (mode) {
    case kModel: return "model";
    case kRules: return "rules";
    default: return "auto";
  }
}

void ArcPredictor::prepare(const float* ua, int n, float fsHz, int8_t* input, ArcWindowInfo* info) {
  OrderStats st = orderStats(ua, n, kNoiseIndex * n / kWindow, gSorted);
  float median = (st.lowMid + st.highMid) * 0.5f;
  float scale = median > kMinScaleUa ? median : kMinScaleUa;
  double noise = static_cast<double>(median - st.noiseQ) / static_cast<double>(scale);
  double threshold = kPulseSigmas * noise > kPulseMinZ ? kPulseSigmas * noise : kPulseMinZ;

  int pulses = 0;
  bool prevAbove = false;
  for (int i = 0; i < n; ++i) {
    float z = (ua[i] - median) / scale;
    if (z > 1.0f) z = 1.0f;
    if (z < -1.0f) z = -1.0f;
    bool above = static_cast<double>(z) > threshold;
    if (above && !prevAbove && i + 1 < n) {  // a pulse: 2 samples or more above the threshold
      float next = (ua[i + 1] - median) / scale;
      if (next > 1.0f) next = 1.0f;
      if (static_cast<double>(next) > threshold) pulses++;
    }
    prevAbove = above;
    if (input != nullptr && n == kWindow) {
      double v = static_cast<double>(z) * 127.0;  // as numpy: z in float64 times 127, rounded half away from 0
      double r = v >= 0 ? floor(v + 0.5) : -floor(-v + 0.5);
      input[i] = static_cast<int8_t>(r > 127 ? 127 : (r < -127 ? -127 : r));
    }
  }
  info->medianUa = median;
  info->noise = static_cast<float>(noise);
  info->pulsesPerS = pulses * fsHz / n;
  info->peakUa = st.peak;
}

int32_t ArcPredictor::networkSum(const int8_t* input, int8_t* features) {
  for (int c = 0; c < ARC_MODEL_C1; ++c) {  // conv 1 -> 8, kernel 7, stride 2, ReLU
    const int8_t* w = kArcW1 + c * ARC_MODEL_K1;
    for (int t = 0; t < kL1; ++t) {
      int32_t acc = kArcB1[c];
      const int8_t* x = input + 2 * t;
      for (int k = 0; k < ARC_MODEL_K1; ++k) acc += static_cast<int32_t>(w[k]) * x[k];
      gLayer1[c][t] = requantRelu(acc, kArcM1, kArcShift1);
    }
  }
  for (int c = 0; c < ARC_MODEL_C2; ++c) {  // conv 8 -> 16, kernel 5, stride 2, ReLU
    const int8_t* w = kArcW2 + c * ARC_MODEL_C1 * ARC_MODEL_K2;
    for (int t = 0; t < kL2; ++t) {
      int32_t acc = kArcB2[c];
      for (int ci = 0; ci < ARC_MODEL_C1; ++ci) {
        const int8_t* x = gLayer1[ci] + 2 * t;
        const int8_t* wc = w + ci * ARC_MODEL_K2;
        for (int k = 0; k < ARC_MODEL_K2; ++k) acc += static_cast<int32_t>(wc[k]) * x[k];
      }
      gLayer2[c][t] = requantRelu(acc, kArcM2, kArcShift2);
    }
  }
  int8_t pooled[kFeatures];
  for (int c = 0; c < ARC_MODEL_C2; ++c) {  // average and maximum over time
    int32_t sum = 0;
    int8_t top = 0;
    for (int t = 0; t < kL2; ++t) {
      sum += gLayer2[c][t];
      if (gLayer2[c][t] > top) top = gLayer2[c][t];
    }
    pooled[c] = static_cast<int8_t>((sum + kL2 / 2) / kL2);
    pooled[ARC_MODEL_C2 + c] = top;
  }
  int32_t acc = kArcB3;  // dense 32 -> 1
  for (int j = 0; j < kFeatures; ++j) acc += static_cast<int32_t>(kArcW3[j]) * pooled[j];
  if (features != nullptr) {
    for (int j = 0; j < kFeatures; ++j) features[j] = pooled[j];
  }
  return acc;
}

float ArcPredictor::modelRisk(int32_t sum) {
  float logit = static_cast<float>(sum) * kArcLogitScale;
  return static_cast<float>(1.0 / (1.0 + exp(-static_cast<double>(logit))));
}

float ArcPredictor::ruleRisk(float pulsesPerS, float noise, float medianUa) {
  double x = kArcRuleA * log1p(static_cast<double>(pulsesPerS)) + kArcRuleC * log(static_cast<double>(noise) + 1e-3) +
             kArcRuleD * log(medianUa > 0.1f ? static_cast<double>(medianUa) : 0.1) + kArcRuleB;
  if (x > 30) x = 30;
  if (x < -30) x = -30;
  return static_cast<float>(1.0 / (1.0 + exp(-x)));
}

float ArcPredictor::predict(const float* ua, int n, float fsHz) {
  ArcWindowInfo info = ArcWindowInfo();
  info.modelRisk = -1.0f;
  info.ruleRisk = -1.0f;
  if (ua == nullptr || n < 16 || n > kWindow || !(fsHz > 0.0f)) {
    last_ = info;
    return -1.0f;
  }
  bool modelFits = n == kWindow && fabsf(fsHz - ARC_MODEL_FS_HZ) <= kFsTolerance * ARC_MODEL_FS_HZ;
  bool runModel = modelFits && mode_ != kRules;
  prepare(ua, n, fsHz, runModel ? input_ : nullptr, &info);
  info.ruleRisk = ruleRisk(info.pulsesPerS, info.noise, info.medianUa);
  int8_t pooled[kFeatures];
  if (runModel) info.modelRisk = modelRisk(networkSum(input_, pooled));
  info.usedModel = runModel;
  // Novelty guard: is this window like the training windows? It does not change the risk (see arc_novelty.h).
  int ties = 0;
  for (int i = 0; i < n; ++i) ties += ua[i] == info.peakUa;
  ArcNovelty nov = ArcNovelty::check(runModel ? pooled : nullptr, info.pulsesPerS, info.noise, info.medianUa, ties);
  info.novelty = nov.reason;
  info.noveltyD2 = nov.distance2;
  last_ = info;
  if (runModel) return info.modelRisk;
  return mode_ == kModel ? -1.0f : info.ruleRisk;
}