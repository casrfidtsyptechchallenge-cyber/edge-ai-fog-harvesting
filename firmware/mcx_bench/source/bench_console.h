#pragma once
#include <stddef.h>
#include <stdint.h>

#include "nullable.h"
#include "safety_core.h"

// The sensor values the bench feeds to the safety core. No sensors are wired yet: you set them by typing,
// so every rule can be tried on the real board. Null = the sensor stopped reporting.
struct BenchInputs {
  Nullable<float> fogIdx;
  Nullable<bool> personNear;
  Nullable<bool> lidClosed;
  Nullable<float> currentUa;
  Nullable<float> arcRisk;  // stands in for the arc predictor (M5a)

  BenchInputs() : fogIdx(0.0f), personNear(false), lidClosed(true), currentUa(0.0f), arcRisk() {}
};

// Commands typed in a serial terminal. Portable C++: it only needs a function that prints text.
class BenchConsole {
 public:
  typedef void (*PrintFn)(const char* text);

  BenchConsole(SafetyCore& safety, BenchInputs& inputs, PrintFn print);
  void feed(int c, uint32_t nowMs);  // one received character; a complete line runs a command
  void tapOperator();                // also used by the SW2 button
  void tapTechnician();              // also used by the SW3 button

 private:
  void run(char* line, uint32_t nowMs);
  void setSensor(const char* name, const char* value);
  void tap(const char* uid, aeroion_role_t role);
  void printState();
  void printInputs();

  SafetyCore& safety_;
  BenchInputs& inputs_;
  PrintFn print_;
  char line_[96];
  size_t length_;
};