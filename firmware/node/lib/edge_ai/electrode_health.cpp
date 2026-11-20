#include "electrode_health.h"

#include <math.h>

ElectrodeHealth::ElectrodeHealth(const HealthConfig& config)
    : config_(config), count_(0), fits_(0), onset_(0), k_(0), lastR2_(0), reference_(0), referenceSet_(false),
      firstOnsets_(0) {}

void ElectrodeHealth::setReference(float onset) {
  referenceSet_ = isfinite(onset) && onset > 0.0f;
  reference_ = referenceSet_ ? onset : 0.0f;
  if (!referenceSet_) {  // learn it again from the next fits
    fits_ = 0;
    firstOnsets_ = 0;
  }
}

Nullable<float> ElectrodeHealth::reference() const {
  if (referenceSet_) return reference_;
  if (fits_ >= config_.referenceFits && config_.referenceFits > 0) return firstOnsets_ / config_.referenceFits;
  return Nullable<float>();
}

Nullable<float> ElectrodeHealth::onset() const { return fits_ > 0 ? Nullable<float>(onset_) : Nullable<float>(); }
Nullable<float> ElectrodeHealth::kCoef() const { return fits_ > 0 ? Nullable<float>(k_) : Nullable<float>(); }

Nullable<float> ElectrodeHealth::score() const {
  Nullable<float> ref = reference();
  if (!ref.has() || fits_ == 0) return Nullable<float>();
  float s = 1.0f - (onset_ - ref.get()) / config_.driftFull;
  return s > 1.0f ? 1.0f : (s < 0.0f ? 0.0f : s);
}

void ElectrodeHealth::drop(uint32_t nowMs) {
  int keep = 0;
  for (int i = 0; i < count_; ++i) {
    if (nowMs - points_[i].ms <= config_.windowMs) points_[keep++] = points_[i];
  }
  count_ = keep;
}

bool ElectrodeHealth::add(float level, float currentUa, bool hvOn, uint32_t nowMs) {
  drop(nowMs);
  if (!hvOn || !isfinite(level) || !isfinite(currentUa) || level <= 0.0f || currentUa < config_.minCurrentUa) {
    return false;
  }
  if (count_ == kMaxPoints) {  // full: the oldest goes
    for (int i = 1; i < count_; ++i) points_[i - 1] = points_[i];
    count_--;
  }
  points_[count_++] = Point{level, currentUa, nowMs};

  float lo = points_[0].level, hi = points_[0].level;
  for (int i = 1; i < count_; ++i) {
    if (points_[i].level < lo) lo = points_[i].level;
    if (points_[i].level > hi) hi = points_[i].level;
  }
  if (count_ < config_.minPoints || hi - lo < config_.minSpan) return false;

  float k, v0, r2;
  bool ok = fit(&k, &v0, &r2);
  lastR2_ = r2;
  if (!ok || r2 < config_.minR2 || !(v0 > 0.0f) || v0 >= lo) return false;  // the onset is below every reading
  onset_ = fits_ == 0 ? v0 : onset_ + config_.onsetAlpha * (v0 - onset_);
  k_ = k;
  if (!referenceSet_ && fits_ < config_.referenceFits) firstOnsets_ += v0;
  fits_++;
  count_ = 0;  // each sweep is used once
  return true;
}

// Least squares of y = I/V on x = V: y = a·x + b, so K = a and V0 = −b/a. In double: a minute of readings, once.
bool ElectrodeHealth::fit(float* k, float* v0, float* r2) const {
  double n = count_, sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
  for (int i = 0; i < count_; ++i) {
    double x = points_[i].level, y = points_[i].currentUa / points_[i].level;
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
    syy += y * y;
  }
  double vx = sxx - sx * sx / n, vy = syy - sy * sy / n, cxy = sxy - sx * sy / n;
  *r2 = 0.0f;
  if (vx <= 0 || vy <= 0) return false;
  double a = cxy / vx, b = (sy - a * sx) / n;
  if (!(a > 0)) return false;
  *k = static_cast<float>(a);
  *v0 = static_cast<float>(-b / a);
  *r2 = static_cast<float>(cxy * cxy / (vx * vy));
  return true;
}