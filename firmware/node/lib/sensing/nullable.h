#pragma once

// A value that may be missing. Missing values become null in the JSON messages
// (contract rule: a missing or failed sensor reports null, never a fake 0).
//
//   Nullable<float> t;     // empty
//   t = 21.5f;             // has a value
//   if (t.has()) use(t.get());
template <typename T>
class Nullable {
 public:
  Nullable() : value_(), has_(false) {}
  Nullable(T value) : value_(value), has_(true) {}  // implicit on purpose, so `t = 21.5f;` works

  bool has() const { return has_; }
  T get() const { return value_; }
  T getOr(T fallback) const { return has_ ? value_ : fallback; }
  void clear() {
    value_ = T();
    has_ = false;
  }

 private:
  T value_;
  bool has_;
};