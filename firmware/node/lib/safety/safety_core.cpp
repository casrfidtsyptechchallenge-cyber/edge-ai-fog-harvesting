#include "safety_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

// States in which the HV is allowed now or may switch on by itself.
bool isActive(aeroion_state_t s) {
  return s == AEROION_STATE_ARMED || s == AEROION_STATE_HARVEST || s == AEROION_STATE_BACKOFF;
}

bool actsAsOperator(aeroion_role_t role) { return role == AEROION_ROLE_OPERATOR || role == AEROION_ROLE_ADMIN; }

}  // namespace

SafetyCore::SafetyCore(const SafetyConfig& config, EventSink& events)
    : config_(config), events_(events), in_(), state_(AEROION_STATE_LOCKED), safeCause_(SafeCause::None),
      level_(0.0f), backoffCap_(0.0f), backoffStartMs_(0), haveLevelTime_(false), lastLevelMs_(0),
      prearmActive_(false), prearmStartMs_(0), prearmDurationMs_(0), havePrediction_(false),
      lastPredictionMs_(0), lastPersonMs_(0), currentLow_(false), currentLowSinceMs_(0), currentHigh_(false), currentHighSinceMs_(0), arcCount_(0), arcNext_(0) {
  NodeEvent::copyText(reason_, sizeof reason_, "power-up");
  maintenanceUid_[0] = '\0';
  memset(arcTimes_, 0, sizeof arcTimes_);
}

bool SafetyCore::hvAllowed() const { return state_ == AEROION_STATE_HARVEST || state_ == AEROION_STATE_BACKOFF; }

bool SafetyCore::prearmed(uint32_t nowMs) const {
  return prearmActive_ && nowMs - prearmStartMs_ < prearmDurationMs_;
}

const char* SafetyCore::blockingFault() const {
  aeroion_fault_t code;
  const char* detail;
  return findFault(&code, &detail) ? detail : nullptr;
}

// ---------------------------------------------------------------- rules run on every update

void SafetyCore::update(const SafetyInputs& in, uint32_t nowMs) {
  in_ = in;
  if (prearmActive_ && !prearmed(nowMs)) prearmActive_ = false;
  if (havePrediction_ && nowMs - lastPredictionMs_ > config_.predictedWindowMs) havePrediction_ = false;

  bool person = in.radarPresence.getOr(false);
  if (person) lastPersonMs_ = nowMs;
  aeroion_fault_t code;
  const char* detail;
  bool faulty = findFault(&code, &detail);

  // 1. Faults and people first, wherever the HV is allowed or could come back by itself.
  bool watching = isActive(state_) || (state_ == AEROION_STATE_SAFE && safeCause_ == SafeCause::Presence);
  if (watching && faulty) {
    enterFault(code, detail);
    return;
  }
  if (isActive(state_) && person) {
    safeCause_ = SafeCause::Presence;
    setState(AEROION_STATE_SAFE, "person detected by radar");
    return;
  }
  if (state_ == AEROION_STATE_SAFE && safeCause_ == SafeCause::Presence) {
    if (!person && nowMs - lastPersonMs_ >= config_.presenceClearMs) {
      setState(AEROION_STATE_ARMED, "area clear");
    }
    return;  // still SAFE: nothing else to decide until the area has been clear long enough
  }

  // 2. Arcs, while the HV is on.
  checkCurrent(nowMs);

  // 3. Normal operation.
  switch (state_) {
    case AEROION_STATE_ARMED:
      if (in.fogPresent) setState(AEROION_STATE_HARVEST, "fog detected");
      break;
    case AEROION_STATE_HARVEST:
      if (!in.fogPresent) {
        setState(AEROION_STATE_ARMED, "fog cleared");
      } else if (riskHigh()) {
        events_.emit(NodeEvent::arcPredicted(in.arcRisk.get(), level_));
        havePrediction_ = true;
        lastPredictionMs_ = nowMs;
        startBackoff("arc predicted", nowMs);
      }
      break;
    case AEROION_STATE_BACKOFF:
      if (nowMs - backoffStartMs_ >= config_.backoffMs && !riskHigh()) {
        setState(in.fogPresent ? AEROION_STATE_HARVEST : AEROION_STATE_ARMED, "backoff over");
      }
      break;
    default:  // LOCKED, SAFE and MAINTENANCE change only on a badge or a command
      break;
  }
}

// ---------------------------------------------------------------- badges and commands

aeroion_badge_result_t SafetyCore::badge(const char* uid, aeroion_role_t role) {
  aeroion_badge_action_t action = AEROION_BADGE_NONE;
  bool granted = false;
  bool faultFree = blockingFault() == nullptr;

  if (role == AEROION_ROLE_TECHNICIAN) {
    if (state_ == AEROION_STATE_MAINTENANCE) {
      action = AEROION_BADGE_MAINT_OFF;
      granted = strcmp(uid, maintenanceUid_) == 0;  // only the badge that locked it out
    } else {
      action = AEROION_BADGE_MAINT_ON;  // from any state, SAFE included: the radar always sees the technician
      granted = true;
    }
  } else if (actsAsOperator(role)) {
    switch (state_) {
      case AEROION_STATE_LOCKED:
        action = AEROION_BADGE_ARM;
        granted = faultFree;
        break;
      case AEROION_STATE_SAFE:
        if (safeCause_ == SafeCause::Presence) {
          action = AEROION_BADGE_DISARM;  // the operator walked up to stop it
          granted = true;
        } else {
          action = AEROION_BADGE_ARM;  // reset after a fault, once its cause is gone
          granted = faultFree;
        }
        break;
      case AEROION_STATE_MAINTENANCE:
        break;  // only the technician can release a lock-out
      default:  // ARMED, HARVEST, BACKOFF
        action = AEROION_BADGE_DISARM;
        granted = true;
        break;
    }
  }  // household and unknown tags: nothing to do at a node

  aeroion_badge_result_t result = granted ? AEROION_BADGE_RESULT_GRANTED : AEROION_BADGE_RESULT_DENIED;
  events_.emit(NodeEvent::badgeTap(uid, role, action, result));
  if (!granted) return result;

  switch (action) {
    case AEROION_BADGE_MAINT_ON:
      NodeEvent::copyText(maintenanceUid_, sizeof maintenanceUid_, uid);
      events_.emit(NodeEvent::maintenance(true, uid));
      setState(AEROION_STATE_MAINTENANCE, "technician lock-out");
      break;
    case AEROION_BADGE_MAINT_OFF:
      events_.emit(NodeEvent::maintenance(false, uid));
      maintenanceUid_[0] = '\0';
      setState(AEROION_STATE_LOCKED, "maintenance over");
      break;
    case AEROION_BADGE_ARM:
      setState(AEROION_STATE_ARMED, state_ == AEROION_STATE_SAFE ? "reset by operator badge" : "operator badge");
      break;
    default:  // DISARM
      setState(AEROION_STATE_LOCKED, "operator badge (disarm)");
      break;
  }
  return result;
}

SafetyReply SafetyCore::lock(const char* reason) {
  if (state_ == AEROION_STATE_MAINTENANCE) {
    return reply(false, "MAINTENANCE: only the technician badge releases it");  // and the HV is off
  }
  char text[65];
  snprintf(text, sizeof text, "station LOCK: %s", reason ? reason : "");
  setState(AEROION_STATE_LOCKED, text);
  return reply(true, "");
}

SafetyReply SafetyCore::prearm(float etaS, uint32_t nowMs) {
  if (state_ != AEROION_STATE_ARMED) {
    char text[65];
    snprintf(text, sizeof text, "state is %s, PREARM needs ARMED", aeroion_state_name(state_));
    return reply(false, text);
  }
  if (!(etaS >= 0.0f && etaS <= 3600.0f)) return reply(false, "eta_s must be 0 to 3600");
  prearmActive_ = true;
  prearmStartMs_ = nowMs;
  prearmDurationMs_ = static_cast<uint32_t>(etaS * 1000.0f) + config_.prearmExtraMs;
  return reply(true, "");
}

void SafetyCore::reportArc(float peakUa, uint32_t nowMs) {
  if (hvAllowed()) handleArc(peakUa, nowMs);
}

// ---------------------------------------------------------------- HV level

float SafetyCore::limitLevel(float requested, uint32_t nowMs) {
  float dtS = haveLevelTime_ ? (nowMs - lastLevelMs_) / 1000.0f : 0.0f;
  haveLevelTime_ = true;
  lastLevelMs_ = nowMs;
  if (!hvAllowed()) {
    level_ = 0.0f;
    return level_;
  }
  float target = fminf(fmaxf(requested, config_.levelMin), config_.levelMax);  // a NaN request gives levelMin
  if (state_ == AEROION_STATE_BACKOFF) target = fminf(target, backoffCap_);
  if (target <= level_) {
    level_ = target;  // down: at once
  } else {
    float from = fmaxf(level_, config_.levelMin);  // switching on starts at the minimum
    level_ = fminf(target, from + config_.rampUpPerS * dtS);
  }
  return level_;
}

// ---------------------------------------------------------------- helpers

bool SafetyCore::findFault(aeroion_fault_t* code, const char** detail) const {
  if (config_.requireRadar && !in_.radarPresence.has()) {
    *code = AEROION_FAULT_SENSOR_FAIL;
    *detail = "radar not reporting";
    return true;
  }
  if (config_.requireLid && !in_.lidClosed.has()) {
    *code = AEROION_FAULT_SENSOR_FAIL;
    *detail = "lid switch not reporting";
    return true;
  }
  if (config_.requireCurrent && !in_.currentUa.has()) {
    *code = AEROION_FAULT_SENSOR_FAIL;
    *detail = "collector current not reporting";
    return true;
  }
  if (in_.lidClosed.has() && !in_.lidClosed.get()) {
    *code = AEROION_FAULT_LID_OPEN;
    *detail = "lid open";
    return true;
  }
  return false;
}

void SafetyCore::enterFault(aeroion_fault_t code, const char* detail) {
  events_.emit(NodeEvent::faultEvent(code, detail));
  setState(AEROION_STATE_SAFE, detail);
  safeCause_ = SafeCause::Fault;  // latched: stays SAFE until an operator badge resets it
}

void SafetyCore::setState(aeroion_state_t to, const char* reason) {
  if (to == state_) return;
  aeroion_state_t from = state_;
  state_ = to;
  if (to != AEROION_STATE_SAFE) safeCause_ = SafeCause::None;
  if (!hvAllowed()) level_ = 0.0f;
  NodeEvent::copyText(reason_, sizeof reason_, reason);
  events_.emit(NodeEvent::stateChange(from, to, reason));
}

void SafetyCore::startBackoff(const char* reason, uint32_t nowMs) {
  backoffStartMs_ = nowMs;
  backoffCap_ = fmaxf(config_.levelMin, level_ - config_.backoffDrop);
  setState(AEROION_STATE_BACKOFF, reason);  // if already in BACKOFF: the timer restarts, lower
}

void SafetyCore::checkCurrent(uint32_t nowMs) {
  if (!hvAllowed() || !in_.currentUa.has()) {
    currentHigh_ = false;
    currentLow_ = false;
    return;
  }
  float ua = in_.currentUa.get();
  // Plausibility: the HV is on well above the corona onset, yet no current at all: the sensor is disconnected.
  if (config_.minCurrentUa > 0.0f && level_ >= config_.minCurrentFromLevel && ua < config_.minCurrentUa) {
    if (!currentLow_) {
      currentLow_ = true;
      currentLowSinceMs_ = nowMs;
    } else if (nowMs - currentLowSinceMs_ >= config_.lowCurrentMs) {
      currentLow_ = false;
      enterFault(AEROION_FAULT_SENSOR_FAIL, "collector current near 0 with the HV on: sensor disconnected?");
      return;
    }
  } else {
    currentLow_ = false;
  }
  if (ua > config_.arcCurrentUa) {
    if (!currentHigh_) {
      currentHigh_ = true;
      currentHighSinceMs_ = nowMs;
      handleArc(ua, nowMs);
    } else if (nowMs - currentHighSinceMs_ >= config_.overcurrentMs) {
      char detail[121];
      snprintf(detail, sizeof detail, "collector current above %lu uA for %lu ms",  // no %f: small C libraries lack it
          static_cast<unsigned long>(config_.arcCurrentUa + 0.5f), static_cast<unsigned long>(config_.overcurrentMs));
      currentHigh_ = false;
      enterFault(AEROION_FAULT_HV_OVERCURRENT, detail);
    }
  } else if (ua < 0.8f * config_.arcCurrentUa) {
    currentHigh_ = false;  // back to normal: the next spike is a new arc
  }
}

void SafetyCore::handleArc(float peakUa, uint32_t nowMs) {
  bool predicted = havePrediction_ && nowMs - lastPredictionMs_ <= config_.predictedWindowMs;
  events_.emit(NodeEvent::arc(peakUa, level_, predicted));

  arcTimes_[arcNext_] = nowMs;
  arcNext_ = (arcNext_ + 1) % kArcHistory;
  if (arcCount_ < kArcHistory) arcCount_++;
  int limit = config_.maxArcs < 1 ? 1 : (config_.maxArcs > kArcHistory ? kArcHistory : config_.maxArcs);
  if (arcCount_ >= limit) {
    uint32_t oldest = arcTimes_[(arcNext_ + kArcHistory - limit) % kArcHistory];
    if (nowMs - oldest <= config_.arcWindowMs) {
      char detail[121];
      snprintf(detail, sizeof detail, "%d arcs within %lu s", limit,
               static_cast<unsigned long>(config_.arcWindowMs / 1000));
      arcCount_ = 0;  // a fresh start after the reset
      enterFault(AEROION_FAULT_HV_OVERCURRENT, detail);
      return;
    }
  }
  startBackoff("arc", nowMs);
}

bool SafetyCore::riskHigh() const { return in_.arcRisk.has() && in_.arcRisk.get() > config_.arcWarnRisk; }

SafetyReply SafetyCore::reply(bool accepted, const char* reason) {
  SafetyReply r;
  r.accepted = accepted;
  NodeEvent::copyText(r.reason, sizeof r.reason, reason);
  return r;
}