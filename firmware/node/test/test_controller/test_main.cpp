// Tests for the HV controller (lib/controller, M6): its search, its arc and risk limits, the station's hints,
// its requests, the hook on JSON telemetry, the gateway that carries the requests to the MCX, and a closed
// loop with the real safety core (M3) and a toy collector that shows whether it beats a fixed voltage.
// They run on the PC: pio test -e native -f test_controller   (add -v to see the closed-loop table)
#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <initializer_list>

#include "controller_hook.h"
#include "hv_controller.h"
#include "node_gateway.h"
#include "rate_estimator.h"
#include "safety_core.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- the controller alone

// A collector whose water per unit of fog peaks at `best`, read once a second. `mass` (g) is its load cell:
// it grows by the rate; pass nullptr for a node without one (then the 30 s rate is used).
static ControllerInput reading(float level, float best, float fog, float risk = 0.0f, float* mass = nullptr) {
  ControllerInput in;
  in.harvest = true;
  in.hvOn = true;
  in.level = level;
  float d = level - best;
  float rate = fog * (1.0f - 8.0f * d * d);  // g/min
  in.rateGpm = rate;
  if (mass != nullptr) {
    *mass += rate / 60.0f;
    in.massG = *mass;
  }
  in.powerW = 0.5f;
  in.arcRisk = risk;
  in.fog = fog;
  in.onset = 0.22f;
  return in;
}

// Runs the controller for `seconds` on that collector (the level follows its target at once).
static float follow(HvController& c, float best, int seconds, uint32_t* nowMs, float fog = 0.7f,
                    float* mass = nullptr) {
  for (int s = 0; s < seconds; ++s) {
    float level = c.target().has() ? c.target().get() : 0.6f;
    c.telemetry(reading(level, best, fog, 0.0f, mass), *nowMs);
    *nowMs += 1000;
  }
  return c.target().get();
}

void test_it_starts_above_the_onset_and_asks_only_while_the_hv_is_on() {
  HvController c;
  float level = 0;
  uint32_t now = 1000;
  TEST_ASSERT_FALSE(c.takeRequest(&level, now));  // nothing known yet
  ControllerInput armed;
  armed.onset = 0.25f;
  c.telemetry(armed, now);  // ARMED: no HV
  TEST_ASSERT_FALSE(c.target().has());
  TEST_ASSERT_FALSE(c.takeRequest(&level, now));
  c.telemetry(reading(0.6f, 0.6f, 0.7f), now);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.60f, c.target().get());  // where the MCX is anyway (onset 0.22 + 0.30 is lower)
  TEST_ASSERT_TRUE(c.takeRequest(&level, now));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.60f, level);
  TEST_ASSERT_FALSE(c.takeRequest(&level, now + 5000));  // unchanged: sent again only every 10 s
  TEST_ASSERT_TRUE(c.takeRequest(&level, now + 10000));
  ControllerInput off = armed;  // the fog cleared: ARMED again
  c.telemetry(off, now + 11000);
  TEST_ASSERT_FALSE(c.takeRequest(&level, now + 30000));
  TEST_ASSERT_TRUE(c.target().has());  // kept for the next fog
  HvController fouled;  // a fouled electrode: the corona starts later, so does the controller
  ControllerInput in = reading(0.6f, 0.6f, 0.7f);
  in.onset = 0.42f;
  fouled.telemetry(in, 0);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.72f, fouled.target().get());
}

void test_it_climbs_to_the_top_and_stays_near_it() {
  HvController c;
  uint32_t now = 0;
  float mass = 0;
  float end = follow(c, 0.78f, 30 * 60, &now, 0.7f, &mass);  // starts at 0.60: the top is 0.18 above
  TEST_ASSERT_FLOAT_WITHIN(0.07f, 0.78f, end);
  float lo = 1, hi = 0;  // then it stays near it: probing one step at a time
  for (int k = 0; k < 40; ++k) {
    float t = follow(c, 0.78f, 90, &now, 0.7f, &mass);
    lo = fminf(lo, t);
    hi = fmaxf(hi, t);
  }
  TEST_ASSERT_TRUE(lo > 0.70f && hi < 0.86f);
  HvController down;  // and down, if the top is lower; this node has no load cell mass: judged on the rate
  now = 0;
  TEST_ASSERT_FLOAT_WITHIN(0.07f, 0.42f, follow(down, 0.42f, 30 * 60, &now));
}

void test_a_tare_during_a_step_is_not_mistaken_for_less_water() {
  // The station tares the load cell (mass back to 0) in the middle of every other dwell: those dwells are
  // judged on the 30 s rate instead, so the search still climbs.
  HvController c;
  uint32_t now = 0;
  float mass = 0;
  for (int s = 0; s < 30 * 60; ++s) {
    if (s % 180 == 120) mass = 0;  // TARE
    float level = c.target().getOr(0.6f);
    c.telemetry(reading(level, 0.78f, 0.7f, 0.0f, &mass), now);
    now += 1000;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.07f, 0.78f, c.target().get());
}

void test_fog_changing_during_a_step_does_not_fool_it() {
  // The fog thins steadily: the water falls whatever the level. Judged per unit of fog, it still finds the top.
  HvController c;
  uint32_t now = 0;
  for (int s = 0; s < 40 * 60; ++s) {
    float fog = 0.95f - 0.6f * s / (40 * 60);
    float level = c.target().has() ? c.target().get() : 0.6f;
    c.telemetry(reading(level, 0.68f, fog), now);
    now += 1000;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.06f, 0.68f, c.target().get());
}

void test_after_an_arc_it_stays_below_and_the_ceiling_rises_again_later() {
  HvController c;
  uint32_t now = 0;
  follow(c, 0.80f, 10, &now);
  c.arc(0.66f, now);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.61f, c.ceiling());
  TEST_ASSERT_TRUE(c.target().get() <= 0.61f + 1e-4f);
  TEST_ASSERT_TRUE(follow(c, 0.80f, 4 * 60, &now) <= 0.61f + 1e-4f);  // the top is higher: it stays below
  follow(c, 0.80f, 7 * 60, &now);  // 11 minutes without arcs
  TEST_ASSERT_TRUE(c.ceiling() > 0.65f);
  TEST_ASSERT_EQUAL_UINT32(1, c.arcs());
}

void test_the_arc_risk_keeps_it_out_of_the_arc_zone() {
  HvController c;
  uint32_t now = 0;
  follow(c, 0.80f, 5, &now);
  float before = c.target().get();
  for (int s = 0; s < 4; ++s, now += 1000) c.telemetry(reading(before, 0.8f, 0.7f, 0.25f), now);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, before, c.target().get());  // 4 readings: not yet
  c.telemetry(reading(before, 0.8f, 0.7f, 0.25f), now);  // 5 readings above 0.20: down one step
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, before - 0.03f, c.target().get());
  TEST_ASSERT_TRUE(c.ceiling() <= before - 0.03f + 1e-4f);
  for (int s = 0; s < 5; ++s) c.telemetry(reading(before - 0.03f, 0.8f, 0.7f, 0.25f), now += 1000);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, before - 0.06f, c.target().get());  // still high at the new level: again
  HvController soft;  // a moderate risk: it may go down, never up
  now = 0;
  float start = follow(soft, 0.9f, 5, &now);
  for (int s = 0; s < 20 * 60; ++s, now += 1000) {
    float level = soft.target().get();
    soft.telemetry(reading(level, 0.9f, 0.7f, 0.15f), now);
  }
  TEST_ASSERT_TRUE(soft.target().get() <= start + 1e-4f);
}

void test_a_station_hint_takes_over_until_it_expires() {
  HvController c;
  uint32_t now = 0;
  follow(c, 0.7f, 10, &now);
  float level;
  TEST_ASSERT_TRUE(c.takeRequest(&level, now));
  c.stationHint(30000, now);  // tools/send_cmd.py hint 0.9 --ttl 30
  TEST_ASSERT_TRUE(c.stationHintActive(now + 29000));
  follow(c, 0.7f, 29, &now);
  TEST_ASSERT_FALSE(c.takeRequest(&level, now));  // the station's level rules meanwhile
  now += 2000;
  TEST_ASSERT_FALSE(c.stationHintActive(now));
  c.telemetry(reading(0.7f, 0.7f, 0.7f), now);
  TEST_ASSERT_TRUE(c.takeRequest(&level, now));  // its own level again
}

void test_off_asks_for_nothing_and_the_water_goal_climbs_higher() {
  HvController c;
  c.setEnabled(false);
  uint32_t now = 0;
  float level;
  c.telemetry(reading(0.6f, 0.6f, 0.7f), now);
  TEST_ASSERT_FALSE(c.takeRequest(&level, now));
  // A collector where more voltage always gives a little more water, but costs much more power: the
  // efficiency goal stops lower than the water goal.
  ControllerConfig water;
  water.goal = ControllerGoal::Water;
  HvController e, w(water);
  uint32_t te = 0, tw = 0;
  for (int s = 0; s < 40 * 60; ++s) {
    for (HvController* x : {&e, &w}) {
      uint32_t& t = x == &e ? te : tw;
      ControllerInput in;
      in.harvest = in.hvOn = true;
      in.level = x->target().has() ? x->target().get() : 0.6f;
      in.fog = 0.7f;
      in.onset = 0.22f;
      in.rateGpm = 0.7f * (1.0f - expf(-6.0f * (in.level - 0.22f)));
      in.powerW = 20.0f * in.level * in.level * in.level;
      x->telemetry(in, t);
      t += 1000;
    }
  }
  TEST_ASSERT_TRUE(w.target().get() > e.target().get() + 0.1f);
}

// ---------------------------------------------------------------- the hook, on JSON

static void telemetryJson(JsonDocument& doc, const char* state, float level, float rate, float risk) {
  doc.clear();
  doc["state"] = state;
  doc["hv_on"] = strcmp(state, "HARVEST") == 0 || strcmp(state, "BACKOFF") == 0;
  doc["hv_level"] = level;
  doc["fog_idx_opt"] = nullptr;  // the optical sensor is out
  doc["fog_idx_virt"] = 0.6f;    // M5b stands in
  doc["rate_gpm"] = rate;
  doc["p_w"] = 1.2f;
  doc["arc_risk"] = risk;
  doc["health"]["onset_level"] = 0.24f;
}

void test_the_hook_reads_the_telemetry_and_the_events() {
  HvController c;
  ControllerHook hook(c);
  JsonDocument doc;
  telemetryJson(doc, "HARVEST", 0.6f, 0.5f, 0.02f);
  doc["health"]["onset_level"] = 0.34f;  // a fouled electrode: start at its onset + 0.30
  hook.telemetry(doc.as<JsonObject>(), 0);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.64f, c.target().get());
  float level;
  uint32_t ttl;
  TEST_ASSERT_TRUE(hook.levelRequest(&level, &ttl, 0));
  TEST_ASSERT_EQUAL_UINT32(30, ttl);
  JsonDocument ev;
  ev["type"] = "ARC_PREDICTED";
  ev["data"]["risk"] = 0.4f;
  ev["data"]["hv_level"] = 0.5f;
  hook.event(ev.as<JsonObjectConst>(), 1000);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.45f, c.ceiling());
  ev.clear();
  ev["type"] = "CMD_ACK";
  ev["data"]["cmd_id"] = "m6-3";
  ev["data"]["result"] = "accepted";
  hook.event(ev.as<JsonObjectConst>(), 1000);
  TEST_ASSERT_EQUAL_UINT32(1, hook.accepted());
  JsonDocument cmd;  // a station SET_HINT sent 10 s ago, valid 30 s: 20 s left
  cmd["type"] = "SET_HINT";
  cmd["ts"] = 1791747040000ULL;
  cmd["ttl_s"] = 30;
  hook.command(cmd.as<JsonObjectConst>(), 1791747050000ULL, 5000);
  TEST_ASSERT_TRUE(c.stationHintActive(24000));
  TEST_ASSERT_FALSE(c.stationHintActive(25000));
}

// ---------------------------------------------------------------- through the gateway

class LinkPort : public GatewayPort {
 public:
  char mcx[8][600];
  int mcxCount = 0;
  int events = 0;
  int acksPublished = 0;
  int lastSeq = 0;
  bool seqGap = false;
  bool mqttConnected() override { return true; }
  bool publish(const char* topic, const char* payload, bool retain) override {
    (void)retain;
    JsonDocument doc;
    if (deserializeJson(doc, payload) == DeserializationError::Ok && doc["seq"].is<int>()) {
      int seq = doc["seq"];
      if (lastSeq != 0 && seq != lastSeq + 1) seqGap = true;
      lastSeq = seq;
    }
    if (strstr(topic, "/event") != nullptr) {
      events++;
      if (strstr(payload, "CMD_ACK") != nullptr) acksPublished++;
    }
    return true;
  }
  void toMcx(const char* line) override {
    if (strstr(line, "\"link\"") != nullptr) return;  // time lines
    snprintf(mcx[mcxCount % 8], sizeof mcx[0], "%s", line);
    mcxCount++;
  }
  void log(const char* text) override { (void)text; }
};

void test_the_gateway_carries_the_requests_and_keeps_their_answers() {
  LinkPort port;
  GatewayConfig config;
  config.nodeId = "n01";
  config.fwVersion = "test";
  config.sensorNames = nullptr;
  config.sensorCount = 0;
  config.replayDelayMs = 0;
  NodeGateway gateway(config, port);
  HvController controller;
  ControllerHook hook(controller);
  gateway.setHook(&hook);
  uint64_t unixMs = 1791747040000ULL;
  uint32_t now = 1000;
  gateway.poll(unixMs, true, now);
  char line[700];
  snprintf(line, sizeof line,
           "{\"topic\":\"aeroion/n01/telemetry\",\"retain\":false,\"payload\":{\"schema_version\":\"1.2\","
           "\"node_id\":\"n01\",\"ts\":%llu,\"seq\":1,\"state\":\"HARVEST\",\"hv_on\":true,\"hv_level\":0.6,"
           "\"fog_idx_opt\":0.7,\"fog_idx_virt\":null,\"t_c\":null,\"rh_pct\":null,\"mass_g\":null,"
           "\"rate_gpm\":0.5,\"i_ua\":40,\"p_w\":1.1,\"e_wh\":null,\"arc_risk\":0.02,\"health\":"
           "{\"onset_level\":0.22,\"k_coef\":null,\"score\":null},\"radar_presence\":false,\"lid_closed\":true,"
           "\"batt_pct\":null}}",
           static_cast<unsigned long long>(unixMs));
  gateway.fromMcx(line, unixMs, now);
  TEST_ASSERT_EQUAL(1, port.mcxCount);  // M6's first request, at once
  JsonDocument sent;
  TEST_ASSERT_TRUE(deserializeJson(sent, port.mcx[0]) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("aeroion/n01/cmd", sent["topic"]);
  TEST_ASSERT_EQUAL_STRING("SET_HINT", sent["payload"]["type"]);
  TEST_ASSERT_EQUAL_STRING("m6-1", sent["payload"]["cmd_id"]);
  TEST_ASSERT_EQUAL(30, sent["payload"]["ttl_s"].as<int>());
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.60f, sent["payload"]["params"]["hv_level"].as<float>());
  TEST_ASSERT_EQUAL_UINT64(unixMs, sent["payload"]["ts"].as<uint64_t>());

  // The MCX's answer is counted, not published, and uses no seq: the station sees no gap.
  snprintf(line, sizeof line,
           "{\"topic\":\"aeroion/n01/event\",\"retain\":false,\"payload\":{\"schema_version\":\"1.2\","
           "\"node_id\":\"n01\",\"ts\":%llu,\"seq\":2,\"type\":\"CMD_ACK\",\"data\":{\"cmd_id\":\"m6-1\","
           "\"result\":\"accepted\",\"reason\":\"\"}}}",
           static_cast<unsigned long long>(unixMs));
  gateway.fromMcx(line, unixMs, now);
  TEST_ASSERT_EQUAL(0, port.acksPublished);
  TEST_ASSERT_EQUAL_UINT32(1, hook.accepted());
  snprintf(line, sizeof line,
           "{\"topic\":\"aeroion/n01/event\",\"retain\":false,\"payload\":{\"schema_version\":\"1.2\","
           "\"node_id\":\"n01\",\"ts\":%llu,\"seq\":3,\"type\":\"ARC\",\"data\":{\"peak_ua\":300,"
           "\"hv_level\":0.58,\"predicted\":false}}}",
           static_cast<unsigned long long>(unixMs));
  gateway.fromMcx(line, unixMs, now);
  TEST_ASSERT_EQUAL(1, port.events);  // the ARC is published...
  TEST_ASSERT_FALSE(port.seqGap);     // ...right after the telemetry: seq 1, 2
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.53f, controller.ceiling());  // ...and M6 saw it

  // A station SET_HINT goes to the MCX as before, and M6 waits until it expires.
  char cmd[300];
  snprintf(cmd, sizeof cmd,
           "{\"schema_version\":\"1.2\",\"cmd_id\":\"st-1\",\"ts\":%llu,\"ttl_s\":20,\"type\":\"SET_HINT\","
           "\"params\":{\"hv_level\":0.9}}",
           static_cast<unsigned long long>(unixMs));
  gateway.fromBroker("aeroion/n01/cmd", cmd, strlen(cmd), unixMs, true, now);
  TEST_ASSERT_EQUAL(2, port.mcxCount);
  TEST_ASSERT_NOT_NULL(strstr(port.mcx[1], "st-1"));
  TEST_ASSERT_TRUE(controller.stationHintActive(now + 19000));
  TEST_ASSERT_EQUAL_UINT32(1, gateway.levelRequests());
}

// ---------------------------------------------------------------- closed loop with the real safety core

const double kPi = 3.14159265358979323846;

// A small random generator, the same on every PC.
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
  double uniform() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 7;
    s_ ^= s_ << 17;
    return (s_ >> 11) * (1.0 / 9007199254740992.0);
  }
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
  double gauss() {
    double u = uniform(), v = uniform();
    return sqrt(-2.0 * log(u + 1e-300)) * cos(2 * kPi * v);
  }

 private:
  uint64_t s_;
};

// A toy collector. Townsend corona current I = K V (V - V0), lowered by fog; charge-driven capture
// 1 - exp(-I / Ic) (Deutsch-Anderson); HV power V x I through a 50 % efficient module (hv_level 1 = 30 kV);
// arcs at random, more and more often near a sparkover level that fog lowers; the electrode may foul (V0
// rises); every node has its own V0, K, sparkover and voltage scale (the real HV module is not chosen yet).
struct Collector {
  double v0, foul, spark, k, ic;
  explicit Collector(Rng& r) {
    double scale = r.uniform(0.85, 1.15);
    v0 = r.uniform(0.15, 0.30) * scale;
    const double fouls[] = {0.0, 0.0, 0.03, 0.06};  // onset rise per hour
    foul = fouls[static_cast<int>(r.uniform() * 4) % 4];
    spark = fmin(0.98, r.uniform(0.62, 0.95) * scale);
    k = r.uniform(120, 200);
    ic = r.uniform(25, 45);
  }
  double onset(double hours) const { return v0 + foul * hours; }
  double hazard(double v, double fog) const { return fmin(2.0, 0.002 * exp((v - (spark - 0.10 * fog)) / 0.03)); }
  double current(double v, double fog, double hours) const {
    double o = onset(hours);
    return v > o ? k * v * (v - o) * (1 - 0.45 * fog) : 0.0;
  }
};

class LoopEvents : public EventSink {
 public:
  HvController* controller = nullptr;
  uint32_t nowMs = 0;
  int arcs = 0, warnings = 0, faults = 0;
  void emit(const NodeEvent& e) override {
    if (e.type == AEROION_EVENT_ARC) arcs++;
    if (e.type == AEROION_EVENT_ARC_PREDICTED) warnings++;
    if (e.type == AEROION_EVENT_FAULT) faults++;
    if (controller != nullptr && (e.type == AEROION_EVENT_ARC || e.type == AEROION_EVENT_ARC_PREDICTED)) {
      controller->arc(e.hvLevel, nowMs);
    }
  }
};

struct LoopResult {
  double gramsPerWh, gramsPerHour, arcsPerHour, warningsPerHour, faultsPerHour;
};

// One node for `minutes`: fixedLevel > 0 asks for that level all the time; otherwise M6 with `goal`.
static LoopResult runNode(uint64_t seed, float fixedLevel, ControllerGoal goal, int minutes = 240) {
  const double baseW = 1.0;
  Rng r(seed);
  Collector col(r);
  double phase = r.uniform(0, 2 * kPi), fogBase = r.uniform(0.5, 0.9);
  SafetyConfig sc;
  sc.arcWarnRisk = 0.30f;  // as the MCX (ARC_WARN_RISK)
  sc.requireRadar = false;
  sc.requireLid = false;
  LoopEvents events;
  SafetyCore safety(sc, events);
  ControllerConfig cc;
  cc.goal = goal;
  HvController controller(cc);
  if (fixedLevel <= 0) events.controller = &controller;
  RateEstimator<30> rate;
  {  // as the firmware: the safety core sees the sensors once before any badge is read
    SafetyInputs first;
    first.radarPresence = false;
    first.lidClosed = true;
    first.currentUa = 0.0f;
    safety.update(first, 0);
  }
  safety.badge("04A1B2C3", AEROION_ROLE_OPERATOR);  // armed
  double mass = 0, water = 0, energyWh = 0;
  float request = fixedLevel > 0 ? fixedLevel : 0.60f;  // the MCX's default until M6 asks
  float risk = 0;
  double current = 0;
  uint32_t faultAtMs = 0;
  bool waiting = false;
  for (int s = 0; s < minutes * 60; ++s) {
    uint32_t now = static_cast<uint32_t>(s) * 1000U;
    events.nowMs = now;
    double hours = s / 3600.0;
    double fog = fmax(0.1, fmin(1.0, fogBase + 0.25 * sin(2 * kPi * s / 900.0 + phase)));
    SafetyInputs in;
    in.fogPresent = true;
    in.radarPresence = false;
    in.lidClosed = true;
    in.currentUa = static_cast<float>(current);
    if (safety.hvAllowed()) in.arcRisk = risk;
    safety.update(in, now);
    if (safety.state() == AEROION_STATE_SAFE && !waiting) {  // FAULT: an operator arms it again 5 min later
      waiting = true;
      faultAtMs = now;
    }
    if (waiting && now - faultAtMs >= 300000) {
      waiting = false;
      safety.badge("04A1B2C3", AEROION_ROLE_OPERATOR);
    }
    float level = safety.limitLevel(request, now);
    bool on = safety.hvAllowed() && level > 0;
    double h = on ? col.hazard(level, fog) : 0.0;
    risk = static_cast<float>(fmin(1.0, (1 - exp(-5 * h)) * exp(0.5 * r.gauss())));  // M5a, noisy
    if (on && r.uniform() < 1 - exp(-h)) safety.reportArc(1000.0f, now);
    current = on ? col.current(level, fog, hours) : 0.0;
    double capture = on ? 0.90 * (1 - exp(-current / col.ic)) : 0.0;
    double gpm = 1.2 * fog * (0.03 + capture);
    double pw = on ? 0.25 + level * 30.0 * current * 1e-3 / 0.5 : 0.0;
    mass += gpm / 60;
    water += gpm / 60;
    energyWh += (baseW + pw) / 3600;
    float massRead = static_cast<float>(mass + 0.05 * r.gauss());  // the load cell, 0.05 g of noise
    rate.add(now, massRead);

    if (fixedLevel > 0) continue;
    ControllerInput ci;
    ci.harvest = safety.state() == AEROION_STATE_HARVEST;
    ci.hvOn = safety.hvAllowed();
    ci.level = level;
    if (rate.ready(10)) ci.rateGpm = rate.perMinute();
    ci.massG = massRead;
    ci.powerW = static_cast<float>(pw);
    if (on) ci.arcRisk = risk;
    ci.fog = static_cast<float>(fmin(1.0, fmax(0.0, fog + 0.02 * r.gauss())));
    ci.onset = static_cast<float>(col.onset(hours) + 0.005 * r.gauss());
    controller.telemetry(ci, now);
    float asked;
    if (controller.takeRequest(&asked, now)) request = asked;
  }
  double h = minutes / 60.0;
  return {water / energyWh, water / h, events.arcs / h, events.warnings / h, events.faults / h};
}

static LoopResult average(float fixedLevel, ControllerGoal goal, uint64_t first, int nodes) {
  LoopResult sum = {0, 0, 0, 0, 0};
  for (int i = 0; i < nodes; ++i) {
    LoopResult x = runNode(first + i, fixedLevel, goal);
    sum.gramsPerWh += x.gramsPerWh / nodes;
    sum.gramsPerHour += x.gramsPerHour / nodes;
    sum.arcsPerHour += x.arcsPerHour / nodes;
    sum.warningsPerHour += x.warningsPerHour / nodes;
    sum.faultsPerHour += x.faultsPerHour / nodes;
  }
  return sum;
}

void test_in_closed_loop_it_matches_the_best_fixed_level_with_far_fewer_arcs() {
  const uint64_t first = 500;
  const int nodes = 24;
  printf("\nClosed loop: %d toy nodes x 4 h, the real safety core (warns at 0.30), 1 W for the node itself\n",
         nodes);
  printf("%-24s %8s %8s %8s %10s %9s\n", "", "g/Wh", "g/h", "arcs/h", "warnings/h", "faults/h");
  const float fixed[] = {0.50f, 0.55f, 0.60f, 0.65f, 0.70f};
  LoopResult f[5];
  for (int i = 0; i < 5; ++i) {
    f[i] = average(fixed[i], ControllerGoal::Efficiency, first, nodes);
    char name[32];
    snprintf(name, sizeof name, "fixed %.2f%s", fixed[i], i == 2 ? " (MCX default)" : "");
    printf("%-24s %8.2f %8.1f %8.1f %10.1f %9.2f\n", name, f[i].gramsPerWh, f[i].gramsPerHour, f[i].arcsPerHour,
           f[i].warningsPerHour, f[i].faultsPerHour);
  }
  LoopResult e = average(0, ControllerGoal::Efficiency, first, nodes);
  LoopResult w = average(0, ControllerGoal::Water, first, nodes);
  printf("%-24s %8.2f %8.1f %8.1f %10.1f %9.2f\n", "M6, goal grams per Wh", e.gramsPerWh, e.gramsPerHour,
         e.arcsPerHour, e.warningsPerHour, e.faultsPerHour);
  printf("%-24s %8.2f %8.1f %8.1f %10.1f %9.2f\n", "M6, goal water", w.gramsPerWh, w.gramsPerHour, w.arcsPerHour,
         w.warningsPerHour, w.faultsPerHour);
  const LoopResult& def = f[2];
  double bestFixed = 0;
  for (int i = 0; i < 5; ++i) bestFixed = fmax(bestFixed, f[i].gramsPerWh);
  TEST_ASSERT_TRUE(e.gramsPerWh > 0.98 * bestFixed);             // as many grams per Wh as the best fixed level...
  TEST_ASSERT_TRUE(e.arcsPerHour < 0.5 * def.arcsPerHour);       // ...with less than half the default's arcs...
  TEST_ASSERT_TRUE(e.faultsPerHour < 0.25 * def.faultsPerHour);  // ...and almost no faults
  for (int i = 0; i < 5; ++i) TEST_ASSERT_TRUE(w.gramsPerHour > f[i].gramsPerHour);  // the most water...
  TEST_ASSERT_TRUE(w.arcsPerHour < 0.5 * f[3].arcsPerHour);  // ...with half the arcs of fixed 0.65
}

// The arc predictor's novelty guard says its windows are unlike its training data: its low risk means little,
// so the controller does not explore upwards; with the guard quiet, the same node climbs.
void test_novel_windows_stop_the_climb() {
  HvController unsure;
  uint32_t now = 0;
  float start = 0;
  for (int s = 0; s < 20 * 60; ++s, now += 1000) {
    float level = unsure.target().has() ? unsure.target().get() : 0.6f;
    ControllerInput in = reading(level, 0.9f, 0.7f);
    in.arcNovel = 0.35f;  // 35 % of the last windows novel
    unsure.telemetry(in, now);
    if (s == 0) start = unsure.target().get();
  }
  TEST_ASSERT_TRUE(unsure.target().get() <= start + 1e-4f);
  HvController sure;
  now = 0;
  float end = follow(sure, 0.9f, 20 * 60, &now);
  TEST_ASSERT_TRUE(end > start + 0.05f);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_it_starts_above_the_onset_and_asks_only_while_the_hv_is_on);
  RUN_TEST(test_it_climbs_to_the_top_and_stays_near_it);
  RUN_TEST(test_a_tare_during_a_step_is_not_mistaken_for_less_water);
  RUN_TEST(test_fog_changing_during_a_step_does_not_fool_it);
  RUN_TEST(test_after_an_arc_it_stays_below_and_the_ceiling_rises_again_later);
  RUN_TEST(test_the_arc_risk_keeps_it_out_of_the_arc_zone);
  RUN_TEST(test_novel_windows_stop_the_climb);
  RUN_TEST(test_a_station_hint_takes_over_until_it_expires);
  RUN_TEST(test_off_asks_for_nothing_and_the_water_goal_climbs_higher);
  RUN_TEST(test_the_hook_reads_the_telemetry_and_the_events);
  RUN_TEST(test_the_gateway_carries_the_requests_and_keeps_their_answers);
  RUN_TEST(test_in_closed_loop_it_matches_the_best_fixed_level_with_far_fewer_arcs);
  return UNITY_END();
}