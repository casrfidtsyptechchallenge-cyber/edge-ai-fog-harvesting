#pragma once
#include <stdint.h>

#include "nullable.h"

// The HV controller (M6), on the ESP32: it searches for the HV level that collects the most water per
// watt-hour, and asks the MCX for it. It only asks: the safety core (M3, on the MCX) decides what is
// allowed, backs off on arcs and warnings, and falls back to its default level if the requests stop.
//
// How it searches (perturb and observe, the usual way to find the top of a curve nobody knows in advance):
//   - hold a level for one dwell (90 s) and judge it on the water collected after the first 30 s: the slope
//     of the load cell's mass over that time (least squares), steadier than its 30 s rate;
//   - move one step (0.03) at a time; once a result is clearly worse than the best of this sweep, go back to
//     the best level, stay there a while, then probe the other side (the best level moves as the fog, the
//     electrode and the air change);
//   - judge the water per unit of fog (rate_gpm / fog index), so fog thickening during a step is not
//     mistaken for the effect of the level.
// What it is judged on: grams per Wh of the whole node (goal Efficiency, the plan's F4), or grams per hour
// (goal Water: the most water the node can collect without arcing).
// It keeps out of the arc zone with the arc predictor (M5a):
//   - arc_risk above riskSoft: no climbing; above riskHigh: down one step at once, before the MCX warns;
//   - arc_novel above novelHold (the predictor sees windows unlike its training data, so its risk cannot be
//     trusted): no climbing; it does not go down for it, the MCX's own arc detection still holds;
//   - after an ARC or ARC_PREDICTED at level L, it stays below L - arcMargin; that ceiling rises slowly
//     again once there has been no arc for a while.
// And never below the corona onset (M5c) + aboveOnset: no corona, no point.
// Plain logic with no hardware: tested on the PC (test/test_controller), with the real safety core.
enum class ControllerGoal : uint8_t { Efficiency, Water };

struct ControllerConfig {
  ControllerGoal goal = ControllerGoal::Efficiency;
  float baseW = 1.0f;               // the node's own power (both chips, sensors), counted in grams per Wh
  uint32_t dwellMs = 90000;         // each level is held this long...
  uint32_t settleMs = 30000;        // ...and judged on the readings after this (the load cell's 30 s window)
  float step = 0.03f;               // one move
  float tolerance = 0.02f;          // a result this much worse (relative) counts as worse
  int holdDwells = 3;               // at the top, stay this many dwells before probing again
  float riskSoft = 0.10f;           // arc_risk above this (averaged): no climbing
  float riskHigh = 0.20f;           // above this: down one step at once (the MCX warns at 0.30)
  int riskReadings = 5;             // the risk is averaged over this many readings
  float novelHold = 0.20f;          // arc_novel above this (the predictor is out of its depth): no climbing
  float arcMargin = 0.05f;          // after an arc or a warning at level L: stay below L minus this
  uint32_t relaxAfterMs = 300000;   // no arc for this long: the ceiling rises again...
  uint32_t relaxEveryMs = 60000;    // ...by relaxStep this often
  float relaxStep = 0.01f;
  float levelMin = 0.30f;           // the safety core's range (safety_config.h)
  float levelMax = 0.95f;
  float aboveOnset = 0.08f;         // never below the corona onset (M5c) plus this
  float startLevel = 0.60f;         // first level: the MCX's default (HV_DEFAULT_LEVEL)...
  float startAboveOnset = 0.30f;    // ...or the onset plus this, if higher (a fouled electrode)
  float minFog = 0.15f;             // readings in thinner fog are not used to judge
  uint32_t refreshMs = 10000;       // the request is sent again this often...
  uint32_t ttlS = 30;               // ...and lasts this long: if the ESP32 stops, the MCX's default comes back
};

// One telemetry reading (once a second), as the ESP32 sees it (lib/controller/controller_hook.h).
struct ControllerInput {
  bool harvest = false;      // state HARVEST: the controller may judge and move
  bool hvOn = false;         // HARVEST or BACKOFF: its requests matter
  float level = 0.0f;        // hv_level, what the safety core allows now
  Nullable<float> massG;     // load cell (ESP32): the water collected
  Nullable<float> rateGpm;   // load cell: its 30 s rate, used when the mass cannot be (a tare in the window)
  Nullable<float> powerW;    // HV module power, INA219 (MCX)
  Nullable<float> arcRisk;   // arc predictor (M5a, MCX)
  Nullable<float> arcNovel;  // its novelty guard: share of recent windows unlike its training data
  Nullable<float> fog;       // fog index: optical, else virtual (M5b)
  Nullable<float> onset;     // corona onset (M5c)
};

class HvController {
 public:
  static const int kMaxRiskReadings = 16;
  static const int kMaxSamples = 180;  // readings judged per dwell (1 a second)

  explicit HvController(const ControllerConfig& config = ControllerConfig());

  void setEnabled(bool on);  // off: no requests (the MCX uses its default level and the station's hints)
  bool enabled() const { return enabled_; }
  void setGoal(ControllerGoal goal);
  ControllerGoal goal() const { return config_.goal; }
  const ControllerConfig& config() const { return config_; }

  void telemetry(const ControllerInput& in, uint32_t nowMs);
  void arc(float level, uint32_t nowMs);              // an ARC or ARC_PREDICTED at this level
  void stationHint(uint32_t validMs, uint32_t nowMs);  // the station sent a SET_HINT: it rules meanwhile
  // True when a level request should go to the MCX now (SET_HINT, valid config().ttlS).
  bool takeRequest(float* level, uint32_t nowMs);

  Nullable<float> target() const { return target_; }  // the level it asks for
  float ceiling() const { return ceiling_; }           // the highest level it will ask for now
  Nullable<float> objective() const { return objective_; }  // the last level's result (g/Wh or g/h, per fog)
  bool stationHintActive(uint32_t nowMs) const;
  uint32_t moves() const { return moves_; }
  uint32_t arcs() const { return arcs_; }
  uint32_t requests() const { return requests_; }
  const char* lastReason() const { return reason_; }  // why the target last changed, for the console

 private:
  float low() const;
  void restartWindow(uint32_t nowMs);
  void moveTo(float level, const char* reason);
  void judge(float riskMean);

  ControllerConfig config_;
  bool enabled_;
  Nullable<float> target_;
  Nullable<float> objective_;
  Nullable<float> best_;        // the best result of this sweep...
  float bestLevel_;             // ...and its level
  float ceiling_;
  int dir_;
  int holding_;
  bool haveOnset_;
  float onset_;
  bool hvOn_;
  bool windowOpen_;
  uint32_t windowStartMs_;
  double sumRate_, sumFog_, sumPower_;
  int samples_;
  float massT_[kMaxSamples];  // seconds since the window started, and the mass then
  float mass_[kMaxSamples];
  int massCount_;
  bool massOk_;
  float novel_;  // the latest arc_novel (0 when unknown)
  float risks_[kMaxRiskReadings];
  int riskCount_;
  int riskNext_;
  bool arcSeen_;
  uint32_t lastArcMs_;
  uint32_t lastRelaxMs_;
  bool hintActive_;
  uint32_t hintStartMs_;
  uint32_t hintValidMs_;
  bool sent_;
  float lastSent_;
  uint32_t lastSentMs_;
  uint32_t moves_;
  uint32_t arcs_;
  uint32_t requests_;
  const char* reason_;
};