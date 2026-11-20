#include "hv_controller.h"

#include <math.h>

namespace {

bool elapsed(uint32_t now, uint32_t since, uint32_t span) {
  return static_cast<int32_t>(now - since) >= static_cast<int32_t>(span);
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

HvController::HvController(const ControllerConfig& config)
    : config_(config), enabled_(true), bestLevel_(0), ceiling_(config.levelMax), dir_(1), holding_(0),
      haveOnset_(false), onset_(0), hvOn_(false), windowOpen_(false), windowStartMs_(0), sumRate_(0), sumFog_(0),
      sumPower_(0), samples_(0), massT_(), mass_(), massCount_(0), massOk_(true), risks_(), novel_(0.0f), riskCount_(0),
      riskNext_(0), arcSeen_(false), lastArcMs_(0), lastRelaxMs_(0), hintActive_(false), hintStartMs_(0),
      hintValidMs_(0), sent_(false), lastSent_(0), lastSentMs_(0), moves_(0), arcs_(0), requests_(0),
      reason_("not started") {
  if (config_.riskReadings < 1) config_.riskReadings = 1;
  if (config_.riskReadings > kMaxRiskReadings) config_.riskReadings = kMaxRiskReadings;
}

void HvController::setEnabled(bool on) {
  enabled_ = on;
  windowOpen_ = false;
  sent_ = false;  // turned on again: the first request goes out at once
}

void HvController::setGoal(ControllerGoal goal) {
  config_.goal = goal;
  best_.clear();  // results in other units: compare afresh
  holding_ = 0;
  windowOpen_ = false;
}

bool HvController::stationHintActive(uint32_t nowMs) const {
  return hintActive_ && !elapsed(nowMs, hintStartMs_, hintValidMs_);
}

void HvController::stationHint(uint32_t validMs, uint32_t nowMs) {
  hintActive_ = validMs > 0;
  hintStartMs_ = nowMs;
  hintValidMs_ = validMs;
  windowOpen_ = false;
  best_.clear();  // the level is the station's meanwhile: start comparing afresh afterwards
}

float HvController::low() const {
  float lo = config_.levelMin;
  if (haveOnset_ && onset_ + config_.aboveOnset > lo) lo = onset_ + config_.aboveOnset;
  return lo > config_.levelMax ? config_.levelMax : lo;
}

void HvController::restartWindow(uint32_t nowMs) {
  windowOpen_ = true;
  windowStartMs_ = nowMs;
  sumRate_ = sumFog_ = sumPower_ = 0;
  samples_ = 0;
  massCount_ = 0;
  massOk_ = true;
}

void HvController::moveTo(float level, const char* reason) {
  float hi = ceiling_;
  float lo = low() < hi ? low() : hi;  // below the arc ceiling first, above the onset if there is room
  float to = clampf(level, lo, hi);
  if (!target_.has() || fabsf(to - target_.get()) > 1e-4f) moves_++;
  target_ = to;
  reason_ = reason;
}

void HvController::arc(float level, uint32_t nowMs) {
  arcs_++;
  arcSeen_ = true;
  lastArcMs_ = nowMs;
  lastRelaxMs_ = nowMs;
  if (!isfinite(level)) level = target_.has() ? target_.get() : config_.startLevel;
  ceiling_ = clampf(level - config_.arcMargin, config_.levelMin, config_.levelMax);
  if (!target_.has() || target_.get() > ceiling_) moveTo(ceiling_, "below the last arc");
  dir_ = -1;
  holding_ = 0;
  best_.clear();
  riskCount_ = 0;
  windowOpen_ = false;
}

void HvController::telemetry(const ControllerInput& in, uint32_t nowMs) {
  if (in.onset.has() && isfinite(in.onset.get())) {
    haveOnset_ = true;
    onset_ = in.onset.get();
  }
  hvOn_ = in.hvOn;
  if (!enabled_) return;
  if (stationHintActive(nowMs) || !in.harvest) {  // the station rules, or no harvest: keep the target
    windowOpen_ = false;
    riskCount_ = 0;
    return;
  }

  // The ceiling left by the last arc rises again slowly.
  if (arcSeen_ && ceiling_ < config_.levelMax && elapsed(nowMs, lastArcMs_, config_.relaxAfterMs) &&
      elapsed(nowMs, lastRelaxMs_, config_.relaxEveryMs)) {
    ceiling_ = clampf(ceiling_ + config_.relaxStep, config_.levelMin, config_.levelMax);
    lastRelaxMs_ = nowMs;
  }

  if (!target_.has()) {
    float start = config_.startLevel;
    if (haveOnset_ && onset_ + config_.startAboveOnset > start) start = onset_ + config_.startAboveOnset;
    moveTo(start, "start");
    restartWindow(nowMs);
    return;
  }
  if (target_.get() < low() && low() <= ceiling_) moveTo(low(), "above the corona onset");  // the onset rose

  novel_ = in.arcNovel.has() && isfinite(in.arcNovel.get()) ? in.arcNovel.get() : 0.0f;
  // The arc predictor's risk, averaged over the last few readings (null counts as 0: no model, no risk).
  float risk = in.arcRisk.has() && isfinite(in.arcRisk.get()) ? in.arcRisk.get() : 0.0f;
  risks_[riskNext_] = risk;
  riskNext_ = (riskNext_ + 1) % config_.riskReadings;
  if (riskCount_ < config_.riskReadings) riskCount_++;
  float riskSum = 0;
  for (int i = 0; i < riskCount_; ++i) riskSum += risks_[i];
  float riskMean = riskSum / riskCount_;
  // Close to the arc zone: down one step, before the MCX has to warn. Then a full set of readings at the new
  // level before the next step down.
  if (riskCount_ == config_.riskReadings && riskMean > config_.riskHigh) {
    ceiling_ = clampf(target_.get() - config_.step, config_.levelMin, ceiling_);
    lastArcMs_ = nowMs;  // the ceiling waits before rising again, as after an arc
    lastRelaxMs_ = nowMs;
    arcSeen_ = true;
    moveTo(ceiling_, "arc risk high");
    dir_ = -1;
    holding_ = 0;
    best_.clear();
    riskCount_ = 0;
    restartWindow(nowMs);
    return;
  }

  if (!windowOpen_) restartWindow(nowMs);
  if (elapsed(nowMs, windowStartMs_, config_.settleMs) && in.rateGpm.has() && in.fog.has() &&
      isfinite(in.rateGpm.get()) && isfinite(in.fog.get()) && in.fog.get() >= config_.minFog) {
    sumRate_ += in.rateGpm.get();
    sumFog_ += in.fog.get();
    sumPower_ += in.powerW.has() && isfinite(in.powerW.get()) ? in.powerW.get() : 0.0f;
    samples_++;
    if (!in.massG.has() || !isfinite(in.massG.get()) || massCount_ >= kMaxSamples ||
        (massCount_ > 0 && in.massG.get() < mass_[massCount_ - 1] - 1.0f)) {
      massOk_ = false;  // no load cell, or a tare (the mass dropped): judge on the rate instead
    } else {
      massT_[massCount_] = (nowMs - windowStartMs_) / 1000.0f;
      mass_[massCount_] = in.massG.get();
      massCount_++;
    }
  }
  if (!elapsed(nowMs, windowStartMs_, config_.dwellMs)) return;
  judge(riskMean);
  restartWindow(nowMs);
}

// The end of a dwell: compare this level's result with the previous one and decide the next level.
void HvController::judge(float riskMean) {
  int expected = static_cast<int>((config_.dwellMs - config_.settleMs) / 1000);
  if (samples_ < expected * 6 / 10 || samples_ < 3) return;  // too few usable readings: hold and try again
  double rate = sumRate_ / samples_, fog = sumFog_ / samples_, power = sumPower_ / samples_;
  if (massOk_ && massCount_ == samples_) {  // the slope of the mass over the window, in g/min
    double st = 0, sm = 0, stt = 0, stm = 0, n = massCount_;
    for (int i = 0; i < massCount_; ++i) {
      st += massT_[i];
      sm += mass_[i];
      stt += static_cast<double>(massT_[i]) * massT_[i];
      stm += static_cast<double>(massT_[i]) * mass_[i];
    }
    double d = n * stt - st * st;
    if (d > 0) rate = (n * stm - st * sm) / d * 60.0;
  }
  double perFog = rate / fog;  // g/min per unit of fog
  float result = static_cast<float>(config_.goal == ControllerGoal::Efficiency
                                        ? perFog * 60.0 / (config_.baseW + power)  // g/Wh
                                        : perFog * 60.0);                          // g/h
  objective_ = result;

  float next = target_.get();
  const char* why = "probing";
  if (holding_ > 0) {
    holding_--;
    why = "holding the best level";
    if (holding_ == 0) {  // a fresh result here, then probe the other side
      best_ = result;
      bestLevel_ = target_.get();
      next = target_.get() + dir_ * config_.step;
      why = "probing again";
    }
  } else if (best_.has() && result < best_.get() * (1.0f - config_.tolerance)) {
    dir_ = -dir_;  // clearly worse: back to the best level of this sweep, and stay there a while
    next = bestLevel_;
    holding_ = config_.holdDwells;
    why = "worse: back to the best level";
  } else {
    if (!best_.has() || result > best_.get()) {
      best_ = result;
      bestLevel_ = target_.get();
    }
    next = target_.get() + dir_ * config_.step;
    why = dir_ > 0 ? "no worse: up" : "no worse: down";
  }
  bool unsure = novel_ > config_.novelHold;  // the predictor is out of its depth: do not explore upwards
  if (next > target_.get() && (riskMean > config_.riskSoft || unsure || next > ceiling_)) {
    next = target_.get() < ceiling_ ? target_.get() : ceiling_;  // not into the arc zone
    dir_ = -1;
    why = riskMean > config_.riskSoft ? "arc risk: not climbing"
                                      : (unsure ? "arc predictor unsure (novel windows): not climbing" : "at the ceiling");
  }
  moveTo(next, why);
  if (target_.get() <= low() + 1e-4f && low() < ceiling_) dir_ = 1;
}

bool HvController::takeRequest(float* level, uint32_t nowMs) {
  if (!enabled_ || !target_.has() || !hvOn_ || stationHintActive(nowMs)) return false;
  bool changed = !sent_ || fabsf(target_.get() - lastSent_) >= 0.005f;
  if (!changed && !elapsed(nowMs, lastSentMs_, config_.refreshMs)) return false;
  sent_ = true;
  lastSent_ = target_.get();
  lastSentMs_ = nowMs;
  requests_++;
  *level = target_.get();
  return true;
}