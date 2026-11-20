// AeroIon-Edge collector node firmware.
//
// M1 (sensing): reads every installed sensor and prints contract messages on the serial port, one JSON
// line each: info at boot, status every 10 s, telemetry every second. Lines for humans start with '#'.
// M3 (safety): every 50 ms the safety core decides whether the high voltage may be on, and how high;
// its events (STATE_CHANGE, FAULT, BADGE_TAP...) go out as they happen.
// Later modules plug in here: M5 edge AI, M6 controller, M7 communication, M4 RFID.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>

#include "aeroion_contract.h"
#include "fog_detector.h"
#include "hv_output.h"
#include "node_clock.h"
#include "node_config.h"
#include "safety_core.h"
#include "sensor_registry.h"
#include "sensors_setup.h"
#include "telemetry_json.h"
#include "watchdog.h"
#if AEROION_BENCH_CONSOLE
#include "bench_console.h"
#endif

static HvOutput hv(PIN_HV_ENABLE, PIN_HV_LEVEL);
static SensorRegistry registry;
static NodeTelemetry telemetry;  // M5 and M6 will fill their parts later
static const NodeIdentity kIdentity = {NODE_ID, NODE_FW_VERSION, NODE_POS_X_M, NODE_POS_Y_M};
static SensorState lastStates[SensorRegistry::kMaxSensors];
static uint32_t seq = 0;
static uint32_t lastTelemetryMs = 0;
static uint32_t lastStatusMs = 0;
static uint32_t lastSafetyMs = 0;
static char lineBuffer[1024];

static void printMessage(const char* kind, bool retain, JsonDocument& payload) {
  char topic[48];
  snprintf(topic, sizeof topic, AEROION_TOPIC_FMT, kIdentity.nodeId, kind);
  if (writeLine(lineBuffer, sizeof lineBuffer, topic, retain, payload.as<JsonObjectConst>()) > 0) {
    Serial.println(lineBuffer);
  } else {
    Serial.println("# ERROR: message larger than lineBuffer");
  }
}

// Events from every module go out at once, numbered with the same seq as the telemetry.
class SerialEventSink : public EventSink {
 public:
  void emit(const NodeEvent& event) override {
    JsonDocument payload;
    buildEventPayload(payload.to<JsonObject>(), kIdentity, nodeTimeMs(), ++seq, event);
    printMessage(AEROION_KIND_EVENT, false, payload);
  }
};

static SafetyConfig makeSafetyConfig() {
  SafetyConfig config;  // defaults: lib/safety/safety_config.h
  config.arcCurrentUa = SAFETY_ARC_CURRENT_UA;
  config.requireRadar = AEROION_FAKE_SENSORS || AEROION_HAS_RADAR;
  config.requireLid = AEROION_FAKE_SENSORS || AEROION_HAS_LID_SWITCH;
  config.requireCurrent = AEROION_FAKE_SENSORS || AEROION_HAS_COLLECTOR_CURRENT;
  return config;
}

static SerialEventSink events;
static const SafetyConfig kSafetyConfig = makeSafetyConfig();
static FogDetector fogDetector(kSafetyConfig, events);
static SafetyCore safety(kSafetyConfig, events);
#if AEROION_BENCH_CONSOLE
static BenchConsole console(safety);
#endif

static void sendInfo() {
  const char* names[SensorRegistry::kMaxSensors];
  int count = registry.count();
  for (int i = 0; i < count; ++i) names[i] = registry.sensorAt(i)->name();
  JsonDocument payload;
  buildInfoPayload(payload.to<JsonObject>(), kIdentity, nodeTimeMs(), names, count);
  printMessage(AEROION_KIND_INFO, true, payload);
}

static void sendStatus(uint32_t nowMs) {
  JsonDocument payload;
  buildStatusPayload(payload.to<JsonObject>(), kIdentity, nodeTimeMs(), nowMs / 1000, nodeTimeSynced());
  printMessage(AEROION_KIND_STATUS, true, payload);
}

static void sendTelemetry(uint32_t nowMs) {
  registry.fillFrame(telemetry.sensors, nowMs);
  JsonDocument payload;
  buildTelemetryPayload(payload.to<JsonObject>(), kIdentity, nodeTimeMs(), ++seq, telemetry);
  printMessage(AEROION_KIND_TELEMETRY, false, payload);
}

// The safety loop: fresh sensor values in, HV on/off and level out.
static void runSafety(uint32_t nowMs) {
  SensorFrame frame;
  registry.fillFrame(frame, nowMs);
  fogDetector.update(frame.fogIdxOpt, AEROION_FOG_SOURCE_OPTICAL, safety.prearmed(nowMs), nowMs);

  SafetyInputs in;
  in.fogPresent = fogDetector.present();
  in.radarPresence = frame.radarPresence;
  in.lidClosed = frame.lidClosed;
  in.currentUa = frame.currentUa;
  in.arcRisk = telemetry.arcRisk;  // the arc predictor (M5a) will fill it; null for now
  safety.update(in, nowMs);

  float level = safety.limitLevel(HV_DEFAULT_LEVEL, nowMs);  // M6 will choose the request; M3 bounds it
  hv.apply(safety.hvAllowed(), level);
  telemetry.state = safety.state();
  telemetry.hvOn = hv.isOn();
  telemetry.hvLevel = level;
}

// Prints every sensor's state at boot, then only the changes (e.g. "SHT45 ok -> stale").
static void reportSensorStates(uint32_t nowMs, bool all) {
  for (int i = 0; i < registry.count(); ++i) {
    SensorState state = registry.stateAt(i, nowMs);
    if (all) {
      Serial.printf("# sensor %-12s %s\n", registry.sensorAt(i)->name(), sensorStateName(state));
    } else if (state != lastStates[i]) {
      Serial.printf("# sensor %-12s %s -> %s\n", registry.sensorAt(i)->name(), sensorStateName(lastStates[i]),
                    sensorStateName(state));
    }
    lastStates[i] = state;
  }
}

void setup() {
  hv.begin();  // first of all: the HV is off before anything else runs
  Serial.begin(115200);
  delay(1500);  // time to open the serial monitor
  Serial.println("# AeroIon-Edge node " NODE_ID ", firmware " NODE_FW_VERSION ", contract " AEROION_SCHEMA_VERSION);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  registerSensors(registry);
  uint32_t now = millis();
  registry.beginAll(now);
  if (registry.count() == 0) Serial.println("# no sensors enabled in node_config.h");
  reportSensorStates(now, true);
  if (!kSafetyConfig.requireRadar) Serial.println("# WARNING: no radar installed: the person cut-off is OFF");
  if (!kSafetyConfig.requireLid) Serial.println("# WARNING: no lid switch installed: the lid interlock is OFF");
  if (!kSafetyConfig.requireCurrent) Serial.println("# WARNING: no collector current sensor: arcs are not detected");

  sendInfo();
  sendStatus(now);
  aeroion_fault_t code;
  const char* detail;
  if (abnormalRestart(&code, &detail)) events.emit(NodeEvent::faultEvent(code, detail));
#if AEROION_BENCH_CONSOLE
  Serial.println("# bench console on: type help");
#endif

  runSafety(now);  // the safety core has inputs before the first badge can arrive
  lastTelemetryMs = now;
  lastStatusMs = now;
  lastSafetyMs = now;
  watchdogBegin(WATCHDOG_TIMEOUT_MS);
}

void loop() {
  watchdogFeed();
  uint32_t now = millis();
  registry.pollAll(now);
#if AEROION_BENCH_CONSOLE
  console.poll(now);
#endif

  if (now - lastSafetyMs >= SAFETY_PERIOD_MS) {
    lastSafetyMs = now;
    runSafety(now);
  }
  if (now - lastTelemetryMs >= TELEMETRY_PERIOD_MS) {
    lastTelemetryMs = now;
    sendTelemetry(now);
    reportSensorStates(now, false);
#if AEROION_DEBUG_RAW
    printRawReadings();
#endif
  }
  if (now - lastStatusMs >= STATUS_PERIOD_MS) {
    lastStatusMs = now;
    sendStatus(now);
  }
  delay(2);
}