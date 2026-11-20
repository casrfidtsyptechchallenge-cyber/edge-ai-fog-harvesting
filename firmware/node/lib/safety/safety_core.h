#pragma once
#include <stdint.h>

#include "aeroion_contract.h"
#include "node_event.h"
#include "nullable.h"
#include "safety_config.h"

// What the safety core looks at. main.cpp refreshes it every SAFETY_PERIOD_MS.
struct SafetyInputs {
  bool fogPresent;               // FogDetector
  Nullable<bool> radarPresence;  // radar (M1)
  Nullable<bool> lidClosed;      // lid switch (M1)
  Nullable<float> currentUa;     // collector current (M1)
  Nullable<float> arcRisk;       // arc predictor (M5a), null until it exists

  SafetyInputs() : fogPresent(false) {}
};

// Answer to a request from the station. M7 will turn it into a CMD_ACK event.
struct SafetyReply {
  bool accepted;
  char reason[65];  // empty when accepted
};

// The node state machine (M3): decides when the high voltage may be on, and how high.
// Plain logic with no hardware access, so every rule is tested on the PC (test/test_safety).
// The AI (M5, M6) and the station can only make requests: this class has the last word.
// The rules are listed in docs/safety-core.md.
class SafetyCore {
 public:
  SafetyCore(const SafetyConfig& config, EventSink& events);

  // Runs the rules with fresh inputs. Call it often (every SAFETY_PERIOD_MS).
  void update(const SafetyInputs& in, uint32_t nowMs);

  // An RFID tag was presented; M4 looks up its role. Sends BADGE_TAP and returns the result.
  // uid must follow the contract: 8 to 20 upper-case hex characters (see isContractUid()).
  aeroion_badge_result_t badge(const char* uid, aeroion_role_t role);

  // Station commands.
  SafetyReply lock(const char* reason);            // LOCK: remote emergency stop
  SafetyReply prearm(float etaS, uint32_t nowMs);  // PREARM: fog is coming

  // An arc seen by another detector (later: M5a's fast current windows).
  void reportArc(float peakUa, uint32_t nowMs);

  // The level the HV module may really use, given what the controller asks for; 0 when the HV must be
  // off. Call it once after every update(): it limits how fast the level rises.
  float limitLevel(float requested, uint32_t nowMs);

  aeroion_state_t state() const { return state_; }
  bool hvAllowed() const;               // true in HARVEST and BACKOFF
  bool prearmed(uint32_t nowMs) const;  // the lower fog threshold applies
  const char* lastReason() const { return reason_; }  // why the state last changed
  const char* blockingFault() const;    // what prevents arming right now, or nullptr

 private:
  enum class SafeCause : uint8_t { None, Presence, Fault };
  static const int kArcHistory = 8;

  bool findFault(aeroion_fault_t* code, const char** detail) const;
  void enterFault(aeroion_fault_t code, const char* detail);
  void setState(aeroion_state_t to, const char* reason);
  void startBackoff(const char* reason, uint32_t nowMs);
  void checkCurrent(uint32_t nowMs);
  void handleArc(float peakUa, uint32_t nowMs);
  bool riskHigh() const;
  static SafetyReply reply(bool accepted, const char* reason);

  SafetyConfig config_;
  EventSink& events_;
  SafetyInputs in_;
  aeroion_state_t state_;
  SafeCause safeCause_;
  char reason_[65];
  char maintenanceUid_[21];
  float level_;
  float backoffCap_;
  uint32_t backoffStartMs_;
  bool haveLevelTime_;
  uint32_t lastLevelMs_;
  bool prearmActive_;
  uint32_t prearmStartMs_;
  uint32_t prearmDurationMs_;
  bool havePrediction_;
  uint32_t lastPredictionMs_;
  uint32_t lastPersonMs_;
  bool currentLow_;          // plausibility: current below minCurrentUa with the HV on...
  uint32_t currentLowSinceMs_;  // ...since then  // last time the radar saw someone (SAFE presence: re-arm only after presenceClearMs)
  bool currentHigh_;
  uint32_t currentHighSinceMs_;
  uint32_t arcTimes_[kArcHistory];  // ring buffer of the latest arcs
  int arcCount_;
  int arcNext_;
};