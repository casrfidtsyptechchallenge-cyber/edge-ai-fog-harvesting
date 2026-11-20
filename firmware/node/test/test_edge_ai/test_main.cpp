// Tests for the edge AI (lib/edge_ai, M5): the arc predictor against the Python reference (golden vectors),
// the electrode health (Townsend fit), the virtual fog sensor, the ESP32's models on real JSON telemetry,
// the arc guard and the wave message. They run on the PC: pio test -e native
#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "arc_guard.h"
#include "arc_model_test.h"
#include "arc_model_data.h"
#include "arc_novelty.h"
#include "arc_novelty_data.h"
#include "arc_predictor.h"
#include "electrode_health.h"
#include "esp_models.h"
#include "node_gateway.h"
#include "virtual_fog.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- arc predictor (M5a)

void test_the_window_is_prepared_as_in_python() {
  for (int w = 0; w < ARC_TEST_WINDOWS; ++w) {
    int8_t input[ArcPredictor::kWindow];
    ArcWindowInfo info;
    ArcPredictor::prepare(kArcTestUa[w], ArcPredictor::kWindow, 10000.0f, input, &info);
    TEST_ASSERT_EQUAL_INT8_ARRAY(kArcTestInput[w], input, ArcPredictor::kWindow);
    TEST_ASSERT_EQUAL_FLOAT(kArcTestPulseRate[w], info.pulsesPerS);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, kArcTestNoise[w], info.noise);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, kArcTestMedian[w], info.medianUa);
  }
}

void test_the_int8_network_gives_exactly_the_python_numbers() {
  for (int w = 0; w < ARC_TEST_WINDOWS; ++w) {
    int8_t features[ArcPredictor::kFeatures];
    TEST_ASSERT_EQUAL_INT32(kArcTestSum[w], ArcPredictor::networkSum(kArcTestInput[w], features));
    TEST_ASSERT_EQUAL_INT8_ARRAY(kArcTestFeatures[w], features, ArcPredictor::kFeatures);
  }
}

void test_the_golden_windows_go_from_safe_to_risky() {
  ArcPredictor p;
  float risks[ARC_TEST_WINDOWS];
  for (int w = 0; w < ARC_TEST_WINDOWS; ++w) {
    risks[w] = p.predict(kArcTestUa[w], ArcPredictor::kWindow, 10000.0f);
    TEST_ASSERT_TRUE(p.last().usedModel);
    TEST_ASSERT_EQUAL_FLOAT(ArcPredictor::modelRisk(kArcTestSum[w]), risks[w]);
  }
  TEST_ASSERT_TRUE(risks[0] < 0.1f);                       // picked from very safe...
  TEST_ASSERT_TRUE(risks[ARC_TEST_WINDOWS - 1] > 0.9f);    // ...to very risky
  for (int w = 1; w < ARC_TEST_WINDOWS; ++w) TEST_ASSERT_TRUE(risks[w] >= risks[w - 1]);
}

void test_the_fallback_takes_over_when_the_model_cannot() {
  ArcPredictor p;
  const float* x = kArcTestUa[ARC_TEST_WINDOWS - 1];
  float model = ArcPredictor::modelRisk(kArcTestSum[ARC_TEST_WINDOWS - 1]);
  TEST_ASSERT_EQUAL_FLOAT(model, p.predict(x, ArcPredictor::kWindow, 9500.0f));  // 5 % off its rate: still the model
  TEST_ASSERT_TRUE(p.last().usedModel);
  TEST_ASSERT_TRUE(p.last().ruleRisk >= 0.0f);  // the rule runs alongside, for the console and the comparison
  float r = p.predict(x, ArcPredictor::kWindow, 7000.0f);  // 30 % off its rate: not the model's job
  TEST_ASSERT_FALSE(p.last().usedModel);
  TEST_ASSERT_TRUE(r >= 0.0f && r <= 1.0f);
  TEST_ASSERT_EQUAL_FLOAT(p.last().ruleRisk, r);
  TEST_ASSERT_FALSE((p.predict(x, 100, 10000.0f), p.last().usedModel));  // a short window: the rule only
  p.setMode(ArcPredictor::kModel);
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, p.predict(x, ArcPredictor::kWindow, 7000.0f));
  p.setMode(ArcPredictor::kRules);
  p.predict(x, ArcPredictor::kWindow, 10000.0f);
  TEST_ASSERT_FALSE(p.last().usedModel);
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, p.predict(nullptr, 256, 10000.0f));
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, p.predict(x, 8, 10000.0f));
}

void test_more_pulses_and_noise_raise_the_rule() {
  TEST_ASSERT_TRUE(ArcPredictor::ruleRisk(200, 0.05f, 60) > ArcPredictor::ruleRisk(10, 0.05f, 60));
  TEST_ASSERT_TRUE(ArcPredictor::ruleRisk(10, 0.15f, 60) > ArcPredictor::ruleRisk(10, 0.05f, 60));
  float r = ArcPredictor::ruleRisk(0, 0, 0);
  TEST_ASSERT_TRUE(r >= 0.0f && r <= 1.0f);
}

void test_the_peak_shows_a_short_arc() {
  float x[ArcPredictor::kWindow];
  for (int i = 0; i < ArcPredictor::kWindow; ++i) x[i] = 40.0f + (i % 7) * 0.3f;
  x[200] = 900.0f;  // a 0.1 ms spike: invisible to the 10 Hz reading
  ArcPredictor p;
  p.predict(x, ArcPredictor::kWindow, 10000.0f);
  TEST_ASSERT_EQUAL_FLOAT(900.0f, p.last().peakUa);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 41.0f, p.last().medianUa);
}

// ---------------------------------------------------------------- electrode health (M5c)

// A sweep of the level from lo to hi over `seconds`, one reading a second, I = K·V·(V − V0)·fogFactor.
static int sweep(ElectrodeHealth& h, float k, float v0, float fogFactor, float lo, float hi, int seconds,
                 uint32_t* nowMs, float noise = 0.0f) {
  int accepted = 0;
  for (int s = 0; s < seconds; ++s) {
    float v = lo + (hi - lo) * s / (seconds - 1);
    float i = k * v * (v - v0) * fogFactor;
    i *= 1.0f + noise * ((s * 7919 % 13) - 6) / 6.0f;  // a repeatable ±noise
    accepted += h.add(v, i, true, *nowMs);
    *nowMs += 1000;
  }
  return accepted;
}

void test_the_townsend_fit_finds_k_and_the_onset_whatever_the_fog() {
  ElectrodeHealth h;
  uint32_t now = 0;
  TEST_ASSERT_FALSE(h.onset().has());
  TEST_ASSERT_EQUAL(1, sweep(h, 160.0f, 0.22f, 0.70f, 0.40f, 0.70f, 12, &now));  // dense fog: 30 % less current
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.22f, h.onset().get());
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 160.0f * 0.70f, h.kCoef().get());
  TEST_ASSERT_TRUE(h.lastR2() > 0.999f);
  sweep(h, 160.0f, 0.22f, 1.0f, 0.45f, 0.75f, 12, &now, 0.02f);  // clear air, 2 % noise
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.22f, h.onset().get());
}

void test_no_fit_without_a_sweep() {
  ElectrodeHealth h;
  uint32_t now = 0;
  TEST_ASSERT_EQUAL(0, sweep(h, 160.0f, 0.22f, 1.0f, 0.60f, 0.65f, 30, &now));  // the level hardly moves
  TEST_ASSERT_FALSE(h.onset().has());
  TEST_ASSERT_FALSE(h.add(0.6f, 50.0f, false, now));                                // HV off
  TEST_ASSERT_FALSE(h.add(0.6f, 0.2f, true, now));                                  // no corona yet
  TEST_ASSERT_FALSE(h.add(NAN, 50.0f, true, now));
  // a slow climb, 0.06 a minute: within any minute the level moves too little, old readings are dropped
  ElectrodeHealth slow;
  now = 0;
  TEST_ASSERT_EQUAL(0, sweep(slow, 160.0f, 0.22f, 1.0f, 0.40f, 0.70f, 300, &now));
}

void test_a_rising_onset_lowers_the_score() {
  ElectrodeHealth h;
  uint32_t now = 0;
  for (int i = 0; i < 3; ++i) sweep(h, 160.0f, 0.22f, 0.8f, 0.40f, 0.70f, 12, &now);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.22f, h.reference().get());  // learned from the first 3 fits
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 1.0f, h.score().get());
  for (int i = 0; i < 20; ++i) sweep(h, 160.0f, 0.295f, 0.8f, 0.45f, 0.75f, 12, &now);  // fouled: +0.075
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.295f, h.onset().get());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.5f, h.score().get());          // half of driftFull (0.15)
  h.setReference(0.20f);
  TEST_ASSERT_FLOAT_WITHIN(0.06f, 0.37f, h.score().get());
  h.setReference(NAN);                                             // cleaned: learn it again
  TEST_ASSERT_FALSE(h.reference().has());
  TEST_ASSERT_FALSE(h.score().has());
}

// ---------------------------------------------------------------- virtual fog sensor (M5b)

void test_the_virtual_fog_learns_from_the_optical_sensor_then_works_alone() {
  VirtualFog vf;
  Nullable<float> onset = 0.22f;
  for (int s = 0; s < 200; ++s) {  // fog from 0.3 to 1.0 and back, the optical sensor working
    float fog = 0.65f + 0.35f * sinf(s * 0.05f);
    float v = 0.60f;
    float i = 160.0f * v * (v - 0.22f) * (1.0f - 0.45f * fog);
    vf.update(v, i, true, onset, Nullable<float>(fog));
  }
  TEST_ASSERT_TRUE(vf.calibrated());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 160.0f, vf.kClear());
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.45f, vf.suppression());
  float v = 0.70f, fog = 0.5f;  // another level, the optical sensor gone
  Nullable<float> est = vf.update(v, 160.0f * v * (v - 0.22f) * (1.0f - 0.45f * fog), true, onset, Nullable<float>());
  TEST_ASSERT_TRUE(est.has());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, est.get());
}

void test_no_virtual_fog_without_what_it_needs() {
  VirtualFog vf;
  Nullable<float> onset = 0.22f;
  for (int s = 0; s < 100; ++s) vf.update(0.6f, 40.0f, true, onset, Nullable<float>(0.5f));  // fog never changed
  TEST_ASSERT_FALSE(vf.calibrated());
  TEST_ASSERT_FALSE(vf.update(0.6f, 40.0f, true, onset, Nullable<float>()).has());
  vf.setCalibration(160.0f, 0.45f);
  TEST_ASSERT_TRUE(vf.update(0.6f, 40.0f, true, onset, Nullable<float>()).has());
  TEST_ASSERT_FALSE(vf.update(0.6f, 40.0f, false, onset, Nullable<float>()).has());               // HV off
  TEST_ASSERT_FALSE(vf.update(0.6f, 40.0f, true, Nullable<float>(), Nullable<float>()).has());    // no onset
  TEST_ASSERT_FALSE(vf.update(0.25f, 40.0f, true, onset, Nullable<float>()).has());               // too near V0
}

// ---------------------------------------------------------------- the ESP32's models on JSON telemetry

static void telemetryJson(JsonDocument& doc, float level, float current, float fogOpt, bool hvOn) {
  doc.clear();
  doc["hv_on"] = hvOn;
  doc["hv_level"] = level;
  doc["i_ua"] = current;
  doc["fog_idx_opt"] = fogOpt;
  doc["fog_idx_virt"] = nullptr;
  JsonObject health = doc["health"].to<JsonObject>();
  health["onset_level"] = nullptr;
  health["k_coef"] = nullptr;
  health["score"] = nullptr;
}

void test_the_esp_models_fill_health_and_virtual_fog() {
  EspModels models;
  JsonDocument doc;
  uint32_t now = 0;
  telemetryJson(doc, 0.0f, 0.1f, 0.5f, false);
  models.telemetry(doc.as<JsonObject>(), now);
  TEST_ASSERT_TRUE(doc["health"]["onset_level"].isNull());  // nothing known yet: the nulls stay
  TEST_ASSERT_TRUE(doc["fog_idx_virt"].isNull());
  for (int s = 0; s < 600; ++s) {  // 10 minutes: the level steps up and down, the fog thickens and thins
    float v = 0.45f + 0.25f * ((s / 6) % 10) / 9.0f;
    float fog = 0.65f + 0.35f * sinf(s * 0.003f);
    telemetryJson(doc, v, 160.0f * v * (v - 0.22f) * (1.0f - 0.45f * fog), fog, true);
    now += 1000;
    models.telemetry(doc.as<JsonObject>(), now);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.22f, doc["health"]["onset_level"].as<float>());
  TEST_ASSERT_FALSE(doc["health"]["score"].isNull());
  TEST_ASSERT_FALSE(doc["fog_idx_virt"].isNull());
  float fog = 0.65f + 0.35f * sinf(599 * 0.003f);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, fog, doc["fog_idx_virt"].as<float>());

  JsonDocument event;
  event["type"] = "MAINT_OFF";
  models.event(event.as<JsonObjectConst>(), now);  // the electrode was serviced
  TEST_ASSERT_TRUE(models.takeReferenceCleared());
  TEST_ASSERT_FALSE(models.takeReferenceCleared());
  TEST_ASSERT_FALSE(models.health().reference().has());
}

// The gateway calls the models on every MCX telemetry line and on every event (lib/gateway, setHook).
class LastPublished : public GatewayPort {
 public:
  char telemetry[1200] = "";
  int telemetryCount = 0;
  bool mqttConnected() override { return true; }
  bool publish(const char* topic, const char* payload, bool retain) override {
    (void)retain;
    if (strstr(topic, "/telemetry") != nullptr) {
      snprintf(telemetry, sizeof telemetry, "%s", payload);
      telemetryCount++;
    }
    return true;
  }
  void toMcx(const char* line) override { (void)line; }
  void log(const char* text) override { (void)text; }
};

void test_the_gateway_runs_the_models_on_the_mcx_telemetry() {
  LastPublished port;
  GatewayConfig config;
  config.nodeId = "n01";
  config.fwVersion = "test";
  config.sensorNames = nullptr;
  config.sensorCount = 0;
  config.replayDelayMs = 0;
  NodeGateway gateway(config, port);
  EspModels models;
  gateway.setHook(&models);
  uint64_t unixMs = 1791747040000ULL;
  uint32_t now = 1000;
  gateway.poll(unixMs, true, now);
  char line[700];
  for (int s = 0; s < 300; ++s) {  // 5 minutes: the level steps up and down, the fog thickens
    float v = 0.45f + 0.25f * ((s / 6) % 10) / 9.0f;
    float fog = 0.4f + 0.5f * s / 300.0f;
    float i = 160.0f * v * (v - 0.22f) * (1.0f - 0.45f * fog);
    snprintf(line, sizeof line,
             "{\"topic\":\"aeroion/n01/telemetry\",\"retain\":false,\"payload\":{\"schema_version\":\"1.2\","
             "\"node_id\":\"n01\",\"ts\":%llu,\"seq\":%d,\"state\":\"HARVEST\",\"hv_on\":true,\"hv_level\":%.4f,"
             "\"fog_idx_opt\":%.3f,\"fog_idx_virt\":null,\"t_c\":null,\"rh_pct\":null,\"mass_g\":null,"
             "\"rate_gpm\":null,\"i_ua\":%.3f,\"p_w\":null,\"e_wh\":null,\"arc_risk\":0.05,\"health\":"
             "{\"onset_level\":null,\"k_coef\":null,\"score\":null},\"radar_presence\":false,\"lid_closed\":true,"
             "\"batt_pct\":null}}",
             static_cast<unsigned long long>(unixMs), s, v, fog, i);
    unixMs += 1000;
    now += 1000;
    gateway.fromMcx(line, unixMs, now);
    gateway.poll(unixMs, true, now);
  }
  TEST_ASSERT_EQUAL(300, port.telemetryCount);
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, port.telemetry) == DeserializationError::Ok);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.22f, doc["health"]["onset_level"].as<float>());
  TEST_ASSERT_TRUE(doc["health"]["score"].as<float>() > 0.9f);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.9f, doc["fog_idx_virt"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.05f, doc["arc_risk"].as<float>());  // the MCX's arc risk passes through

  snprintf(line, sizeof line,
           "{\"topic\":\"aeroion/n01/event\",\"retain\":false,\"payload\":{\"schema_version\":\"1.2\","
           "\"node_id\":\"n01\",\"ts\":%llu,\"seq\":400,\"type\":\"MAINT_OFF\",\"data\":{\"uid\":\"0E0E0E0E\"}}}",
           static_cast<unsigned long long>(unixMs));
  gateway.fromMcx(line, unixMs, now);
  TEST_ASSERT_TRUE(models.takeReferenceCleared());  // the technician's lock-out ended: the electrode was serviced
}

// ---------------------------------------------------------------- arc guard and wave message

class FakeSource : public ArcWindowSource {
 public:
  float rate = 10000.0f;
  int calls = 0;
  float capture(float* ua, int n, float fsHz) override {
    (void)fsHz;
    calls++;
    for (int i = 0; i < n; ++i) ua[i] = kArcTestUa[ARC_TEST_WINDOWS - 1][i];
    return rate;
  }
};

void test_the_guard_judges_a_window_every_period_while_the_hv_is_on() {
  FakeSource source;
  ArcGuard guard(source, 100);
  TEST_ASSERT_FALSE(guard.poll(false, 0));
  TEST_ASSERT_FALSE(guard.risk().has());
  TEST_ASSERT_TRUE(guard.poll(true, 50));
  TEST_ASSERT_EQUAL_FLOAT(ArcPredictor::modelRisk(kArcTestSum[ARC_TEST_WINDOWS - 1]), guard.risk().get());
  TEST_ASSERT_FALSE(guard.poll(true, 100));  // too soon
  TEST_ASSERT_TRUE(guard.poll(true, 150));
  TEST_ASSERT_EQUAL(2, source.calls);
  TEST_ASSERT_EQUAL(256, guard.length());
  TEST_ASSERT_EQUAL_UINT32(150, guard.windowMs());
  source.rate = 0.0f;                         // the ADC failed
  TEST_ASSERT_FALSE(guard.poll(true, 250));
  TEST_ASSERT_FALSE(guard.risk().has());
  TEST_ASSERT_EQUAL_UINT32(1, guard.failures());
  source.rate = 10000.0f;
  guard.poll(true, 350);
  TEST_ASSERT_TRUE(guard.risk().has());
  guard.poll(false, 360);                     // HV off: no risk
  TEST_ASSERT_FALSE(guard.risk().has());
}

void test_the_wave_line_is_a_contract_message() {
  static char line[3072];
  float ua[256];
  for (int i = 0; i < 256; ++i) ua[i] = 37.25f + i * 0.5f;
  ua[3] = -1.24f;  // a small negative offset prints as such
  size_t len = formatWaveLine(line, sizeof line, "n01", "1.2", 1791747040265ULL, 42, "ARC_PREDICTED", 9987, ua, 256);
  TEST_ASSERT_TRUE(len > 0 && len < sizeof line);
  TEST_ASSERT_EQUAL(strlen(line), len);
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, line) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("aeroion/n01/wave", doc["topic"]);
  JsonObject p = doc["payload"];
  TEST_ASSERT_EQUAL_STRING("ARC_PREDICTED", p["trigger"]);
  TEST_ASSERT_EQUAL(9987, p["fs_hz"].as<int>());
  TEST_ASSERT_EQUAL_STRING("uA", p["unit"]);
  TEST_ASSERT_EQUAL_UINT64(1791747040265ULL, p["ts"].as<uint64_t>());
  TEST_ASSERT_EQUAL(256, p["samples"].size());
  TEST_ASSERT_FLOAT_WITHIN(0.051f, 37.3f, p["samples"][0].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.2f, p["samples"][3].as<float>());
  // the time is printed in two parts (no %llu on the MCX): zeros inside it and small values stay exact
  TEST_ASSERT_TRUE(formatWaveLine(line, sizeof line, "n01", "1.2", 1791000000007ULL, 1, "ARC", 10000, ua, 256) > 0);
  TEST_ASSERT_NOT_NULL(strstr(line, "\"ts\":1791000000007,"));
  TEST_ASSERT_TRUE(formatWaveLine(line, sizeof line, "n01", "1.2", 5ULL, 1, "ARC", 10000, ua, 256) > 0);
  TEST_ASSERT_NOT_NULL(strstr(line, "\"ts\":5,"));
  char small[200];
  TEST_ASSERT_EQUAL(0, formatWaveLine(small, sizeof small, "n01", "1.2", 1, 1, "ARC", 10000, ua, 256));
}

// ---------------------------------------------------------------- novelty guard (M5a)

void test_the_novelty_guard_belongs_to_the_model_in_the_firmware() {
  // python -m ml arc-train / pipeline / models --deploy write both headers: they must name the same model
  TEST_ASSERT_EQUAL_STRING(ARC_MODEL_NAME, ARC_NOVELTY_MODEL);
}

void test_a_disconnected_sensor_is_out_of_range() {
  float w[ArcPredictor::kWindow];
  uint32_t seed = 12345;  // amplifier noise around 0.3 µA, never twice the same value
  for (int i = 0; i < ArcPredictor::kWindow; ++i) {
    seed = seed * 1664525u + 1013904223u;
    w[i] = 0.1f + 0.4f * static_cast<float>(seed >> 8) / 16777216.0f;
  }
  ArcPredictor p;
  p.predict(w, ArcPredictor::kWindow, 10000.0f);
  TEST_ASSERT_EQUAL(ArcNovelty::kOutOfRange, p.last().novelty);
}

void test_a_clipped_input_is_saturated() {
  float w[ArcPredictor::kWindow];
  for (int i = 0; i < ArcPredictor::kWindow; ++i) w[i] = kArcTestUa[2][i] > 44.0f ? 44.0f : kArcTestUa[2][i];
  ArcPredictor p;
  p.predict(w, ArcPredictor::kWindow, 10000.0f);
  TEST_ASSERT_EQUAL(ArcNovelty::kSaturated, p.last().novelty);
}

void test_the_guard_never_changes_the_risk() {
  for (int k = 0; k < ARC_TEST_WINDOWS; ++k) {
    ArcPredictor p;
    float risk = p.predict(kArcTestUa[k], ArcPredictor::kWindow, 10000.0f);
    TEST_ASSERT_EQUAL_FLOAT(ArcPredictor::modelRisk(kArcTestSum[k]), risk);
    TEST_ASSERT_TRUE(p.last().noveltyD2 >= 0.0f && isfinite(p.last().noveltyD2));
    TEST_ASSERT_TRUE(p.last().novelty == ArcNovelty::kKnown || p.last().novelty == ArcNovelty::kUnusualShape);
  }
}

void test_the_shape_distance_uses_the_pooled_features() {
  float d0 = ArcNovelty::shapeDistance2(kArcTestFeatures[0]);
  ArcNovelty n = ArcNovelty::check(kArcTestFeatures[0], 100.0f, 0.02f, 50.0f, 1);
  TEST_ASSERT_EQUAL_FLOAT(d0, n.distance2);
  TEST_ASSERT_EQUAL(d0 > ArcNovelty::threshold() ? ArcNovelty::kUnusualShape : ArcNovelty::kKnown, n.reason);
  ArcNovelty none = ArcNovelty::check(nullptr, 100.0f, 0.02f, 50.0f, 1);  // the CNN did not run
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, none.distance2);
}

// A source whose windows are clipped (ADC at full scale): every window novel, the share goes to 1.
class ClippedSource : public ArcWindowSource {
 public:
  bool clip = true;
  float capture(float* ua, int n, float fsHz) override {
    (void)fsHz;
    for (int i = 0; i < n; ++i) {
      float v = kArcTestUa[2][i];
      ua[i] = clip && v > 44.0f ? 44.0f : v;
    }
    return 10000.0f;
  }
};

void test_the_guard_reports_the_share_of_novel_windows() {
  ClippedSource source;
  ArcGuard guard(source, 100);
  TEST_ASSERT_FALSE(guard.novelShare().has());  // no window yet
  uint32_t now = 0;
  for (int k = 0; k < 10; ++k, now += 100) guard.poll(true, now);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, guard.novelShare().get());
  TEST_ASSERT_TRUE(guard.lastNovel());
  TEST_ASSERT_TRUE(guard.risk().has());  // the risk is still given out
  source.clip = false;
  for (int k = 0; k < 10; ++k, now += 100) guard.poll(true, now);
  TEST_ASSERT_EQUAL_FLOAT(0.5f, guard.novelShare().get());  // 10 of the last 20
  TEST_ASSERT_EQUAL_UINT32(10, guard.novelWindows());
  guard.poll(false, now);  // HV off: no share, a fresh start next time
  TEST_ASSERT_FALSE(guard.novelShare().has());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_window_is_prepared_as_in_python);
  RUN_TEST(test_the_int8_network_gives_exactly_the_python_numbers);
  RUN_TEST(test_the_golden_windows_go_from_safe_to_risky);
  RUN_TEST(test_the_fallback_takes_over_when_the_model_cannot);
  RUN_TEST(test_more_pulses_and_noise_raise_the_rule);
  RUN_TEST(test_the_peak_shows_a_short_arc);
  RUN_TEST(test_the_novelty_guard_belongs_to_the_model_in_the_firmware);
  RUN_TEST(test_a_disconnected_sensor_is_out_of_range);
  RUN_TEST(test_a_clipped_input_is_saturated);
  RUN_TEST(test_the_guard_never_changes_the_risk);
  RUN_TEST(test_the_shape_distance_uses_the_pooled_features);
  RUN_TEST(test_the_townsend_fit_finds_k_and_the_onset_whatever_the_fog);
  RUN_TEST(test_no_fit_without_a_sweep);
  RUN_TEST(test_a_rising_onset_lowers_the_score);
  RUN_TEST(test_the_virtual_fog_learns_from_the_optical_sensor_then_works_alone);
  RUN_TEST(test_no_virtual_fog_without_what_it_needs);
  RUN_TEST(test_the_esp_models_fill_health_and_virtual_fog);
  RUN_TEST(test_the_gateway_runs_the_models_on_the_mcx_telemetry);
  RUN_TEST(test_the_guard_judges_a_window_every_period_while_the_hv_is_on);
  RUN_TEST(test_the_guard_reports_the_share_of_novel_windows);
  RUN_TEST(test_the_wave_line_is_a_contract_message);
  return UNITY_END();
}