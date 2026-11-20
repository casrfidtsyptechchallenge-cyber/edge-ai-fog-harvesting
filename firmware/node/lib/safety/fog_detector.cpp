#include "fog_detector.h"

FogDetector::FogDetector(const SafetyConfig& config, EventSink& events)
    : config_(config), events_(events), present_(false), sinceMs_(0) {}

void FogDetector::update(Nullable<float> fogIdx, aeroion_fog_source_t source, bool prearmed, uint32_t nowMs) {
  float on = prearmed ? config_.fogOnPrearmed : config_.fogOn;
  if (!present_ && fogIdx.has() && fogIdx.get() > on) {
    present_ = true;
    sinceMs_ = nowMs;
    events_.emit(NodeEvent::fogDetected(fogIdx.get(), source));
  } else if (present_ && (!fogIdx.has() || fogIdx.get() < config_.fogOff)) {
    present_ = false;
    events_.emit(NodeEvent::fogCleared((nowMs - sinceMs_) / 1000.0f));
  }
}