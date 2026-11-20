#include "virtual_fog.h"

#include <math.h>

VirtualFog::VirtualFog(const VirtualFogConfig& config)
    : config_(config), sw_(0), sf_(0), sk_(0), sff_(0), sfk_(0), kClear_(0), suppression_(0), fitted_(false),
      fixed_(false) {}

void VirtualFog::setCalibration(float kClear, float suppression) {
  fixed_ = kClear > 0.0f && suppression > 0.0f && suppression <= 1.0f;
  if (fixed_) {
    kClear_ = kClear;
    suppression_ = suppression;
  }
}

bool VirtualFog::calibrated() const { return fitted_ || fixed_; }

void VirtualFog::refit() {
  if (sw_ < config_.minWeight) return;
  double mf = sf_ / sw_, mk = sk_ / sw_;
  double vf = sff_ / sw_ - mf * mf, cfk = sfk_ / sw_ - mf * mk;
  if (vf < config_.minFogSpread * config_.minFogSpread) return;
  double slope = cfk / vf, k0 = mk - slope * mf;  // k = k0 + slope·fog, and slope = −K·s
  if (!(k0 > 0) || !(slope < 0)) return;
  double s = -slope / k0;
  if (s > 1.0) return;
  kClear_ = static_cast<float>(k0);
  suppression_ = static_cast<float>(s);
  fitted_ = true;
}

Nullable<float> VirtualFog::update(float level, float currentUa, bool hvOn, Nullable<float> onset,
                                   Nullable<float> fogOpt) {
  if (!hvOn || !onset.has() || !isfinite(level) || !isfinite(currentUa) || currentUa < config_.minCurrentUa ||
      level < onset.get() + config_.minAboveOnset) {
    return Nullable<float>();
  }
  double k = currentUa / (static_cast<double>(level) * (level - onset.get()));
  if (fogOpt.has() && isfinite(fogOpt.get())) {
    double f = fogOpt.get();
    sw_ = sw_ * config_.forget + 1;
    sf_ = sf_ * config_.forget + f;
    sk_ = sk_ * config_.forget + k;
    sff_ = sff_ * config_.forget + f * f;
    sfk_ = sfk_ * config_.forget + f * k;
    if (!fixed_) refit();
  }
  if (!calibrated()) return Nullable<float>();
  float fog = static_cast<float>((1.0 - k / kClear_) / suppression_);
  return fog > 1.0f ? 1.0f : (fog < 0.0f ? 0.0f : fog);
}