#include "arc_guard.h"

#include <math.h>
#include <stdio.h>

ArcGuard::ArcGuard(ArcWindowSource& source, uint32_t periodMs, MicrosFn micros)
    : source_(source), periodMs_(periodMs), micros_(micros), window_(), length_(0), fsHz_(0), started_(false),
      lastMs_(0), windowMs_(0), windows_(0), failures_(0), captureUs_(0), inferenceUs_(0), novel_(),
      novelCount_(0), novelNext_(0), lastNovel_(false), novelWindows_(0) {}

Nullable<float> ArcGuard::novelShare() const {
  if (novelCount_ == 0) return Nullable<float>();
  int n = 0;
  for (int i = 0; i < novelCount_; ++i) n += novel_[i];
  return static_cast<float>(n) / static_cast<float>(novelCount_);
}

bool ArcGuard::poll(bool hvOn, uint32_t nowMs) {
  if (!hvOn) {
    risk_.clear();
    started_ = false;
    novelCount_ = 0;  // a fresh share when the HV comes back
    novelNext_ = 0;
    lastNovel_ = false;
    return false;
  }
  if (started_ && nowMs - lastMs_ < periodMs_) return false;
  started_ = true;
  lastMs_ = nowMs;
  uint32_t t0 = micros_ ? micros_() : 0;
  float fs = source_.capture(window_, ArcPredictor::kWindow, ArcPredictor::modelSampleRateHz());
  uint32_t t1 = micros_ ? micros_() : 0;
  if (!(fs > 0.0f)) {  // no window: no risk, rather than an old one
    failures_++;
    risk_.clear();
    return false;
  }
  float r = predictor_.predict(window_, ArcPredictor::kWindow, fs);
  uint32_t t2 = micros_ ? micros_() : 0;
  captureUs_ = t1 - t0;
  inferenceUs_ = t2 - t1;
  length_ = ArcPredictor::kWindow;
  fsHz_ = fs;
  windowMs_ = nowMs;
  windows_++;
  lastNovel_ = predictor_.last().novelty != 0;
  if (lastNovel_) novelWindows_++;
  novel_[novelNext_] = lastNovel_ ? 1 : 0;
  novelNext_ = (novelNext_ + 1) % kNoveltySpan;
  if (novelCount_ < kNoveltySpan) novelCount_++;
  if (r >= 0.0f) {
    risk_ = r;
  } else {
    risk_.clear();
  }
  return true;
}

size_t formatWaveLine(char* out, size_t cap, const char* nodeId, const char* schemaVersion, uint64_t ts,
                      uint32_t seq, const char* trigger, int fsHz, const float* ua, int n) {
  if (out == nullptr || cap == 0 || ua == nullptr || n < 1) return 0;
  // No %llu: the MCX's small C library prints it as "lu" and then reads the wrong arguments. The 64-bit time is
  // printed in two parts instead (milliseconds since 1970: about 1.8e12, so the high part is never zero today).
  char tsText[24];
  unsigned long tsHigh = static_cast<unsigned long>(ts / 1000000000ULL);
  unsigned long tsLow = static_cast<unsigned long>(ts % 1000000000ULL);
  if (tsHigh > 0) {
    snprintf(tsText, sizeof tsText, "%lu%09lu", tsHigh, tsLow);
  } else {
    snprintf(tsText, sizeof tsText, "%lu", tsLow);
  }
  int len = snprintf(out, cap,
                     "{\"topic\":\"aeroion/%s/wave\",\"retain\":false,\"payload\":{\"schema_version\":\"%s\","
                     "\"node_id\":\"%s\",\"ts\":%s,\"seq\":%lu,\"trigger\":\"%s\",\"fs_hz\":%d,\"unit\":\"uA\","
                     "\"samples\":[",
                     nodeId, schemaVersion, nodeId, tsText, static_cast<unsigned long>(seq), trigger, fsHz);
  if (len < 0 || static_cast<size_t>(len) >= cap) return 0;
  size_t used = static_cast<size_t>(len);
  for (int i = 0; i < n; ++i) {
    // tenths of a µA as an integer, so no %f is needed (small C libraries lack it)
    float v = isfinite(ua[i]) ? ua[i] : 0.0f;
    long tenths = lroundf(v * 10.0f);
    unsigned long mag = static_cast<unsigned long>(tenths < 0 ? -tenths : tenths);
    len = snprintf(out + used, cap - used, "%s%s%lu.%lu", i ? "," : "", tenths < 0 ? "-" : "", mag / 10, mag % 10);
    if (len < 0 || static_cast<size_t>(len) >= cap - used) return 0;
    used += static_cast<size_t>(len);
  }
  len = snprintf(out + used, cap - used, "]}}");
  if (len < 0 || static_cast<size_t>(len) >= cap - used) return 0;
  return used + static_cast<size_t>(len);
}