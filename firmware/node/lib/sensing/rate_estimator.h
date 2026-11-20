#pragma once
#include <stdint.h>

// Slope of a value over its last N samples (least squares), in units per minute.
// Turns the load-cell mass (g) into a collection rate (g/min). Averaging over a window makes it
// robust to the noise of single readings. Fixed size, no dynamic memory.
template <int N>
class RateEstimator {
 public:
  RateEstimator() : count_(0), next_(0) {}

  void reset() {
    count_ = 0;
    next_ = 0;
  }

  void add(uint32_t timeMs, float value) {
    times_[next_] = timeMs;
    values_[next_] = value;
    next_ = (next_ + 1) % N;
    if (count_ < N) count_++;
  }

  bool ready(int minSamples = 5) const { return count_ >= minSamples; }

  float perMinute() const {
    if (count_ < 2) return 0.0f;
    // Times relative to the newest sample, in seconds, keep the sums small and survive millis() wrapping.
    uint32_t newest = times_[(next_ + N - 1) % N];
    double st = 0, sv = 0, stt = 0, stv = 0;
    for (int i = 0; i < count_; ++i) {
      double t = -static_cast<double>(static_cast<uint32_t>(newest - times_[i])) / 1000.0;
      double v = values_[i];
      st += t;
      sv += v;
      stt += t * t;
      stv += t * v;
    }
    double n = count_;
    double denominator = n * stt - st * st;
    if (denominator <= 0) return 0.0f;
    return static_cast<float>((n * stv - st * sv) / denominator * 60.0);
  }

 private:
  uint32_t times_[N];
  float values_[N];
  int count_;
  int next_;
};