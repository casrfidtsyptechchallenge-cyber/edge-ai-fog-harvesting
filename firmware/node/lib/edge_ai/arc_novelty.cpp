#include "arc_novelty.h"

#include <math.h>

#include "arc_model_data.h"
#include "arc_novelty_data.h"

static_assert(ARC_NOVELTY_DIMS == 2 * ARC_MODEL_C2, "arc_novelty_data.h does not match the model's features");

const char* ArcNovelty::reasonName(Reason r) {
  switch (r) {
    case kSaturated: return "saturated";
    case kOutOfRange: return "out of range";
    case kUnusualShape: return "unusual shape";
    default: return "known";
  }
}

float ArcNovelty::threshold() { return ARC_NOVELTY_THRESHOLD; }

float ArcNovelty::shapeDistance2(const int8_t* pooled) {
  float g[ARC_NOVELTY_DIMS];
  for (int j = 0; j < ARC_NOVELTY_DIMS; ++j) {
    g[j] = log1pf(static_cast<float>(pooled[j] > 0 ? pooled[j] : 0)) - kNoveltyMean[j];  // ReLU outputs: >= 0
  }
  float d2 = 0.0f;
  for (int i = 0; i < ARC_NOVELTY_DIMS; ++i) {  // W is upper-triangular: row i starts at column i
    const float* w = kNoveltyWhiten + i * ARC_NOVELTY_DIMS;
    float s = 0.0f;
    for (int j = i; j < ARC_NOVELTY_DIMS; ++j) s += w[j] * g[j];
    d2 += s * s;
  }
  return d2;
}

ArcNovelty ArcNovelty::check(const int8_t* pooled, float pulsesPerS, float noise, float medianUa, int maxTies) {
  ArcNovelty r;
  r.reason = kKnown;
  r.distance2 = pooled != nullptr ? shapeDistance2(pooled) : -1.0f;
  if (maxTies >= ARC_NOVELTY_SATURATION_TIES) {
    r.reason = kSaturated;
    return r;
  }
  // the rule's inputs, computed as in ArcPredictor::ruleRisk
  const float phys[3] = {log1pf(pulsesPerS), logf(noise + 1e-3f), logf(medianUa > 0.1f ? medianUa : 0.1f)};
  for (int k = 0; k < 3; ++k) {
    if (!(phys[k] >= kNoveltyPhysLo[k] && phys[k] <= kNoveltyPhysHi[k])) {  // NaN counts as out of range
      r.reason = kOutOfRange;
      return r;
    }
  }
  if (pooled != nullptr && r.distance2 > ARC_NOVELTY_THRESHOLD) r.reason = kUnusualShape;
  return r;
}
