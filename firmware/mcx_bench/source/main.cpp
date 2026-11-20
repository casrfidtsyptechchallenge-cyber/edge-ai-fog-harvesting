// AeroIon-Edge M3 test bench on the NXP FRDM-MCXA156.
//
// Runs the ESP32 node's safety core (firmware/node/lib/safety) unchanged on real hardware:
//   SW2 = operator badge, SW3 = technician badge, the RGB LED shows the state,
//   sensor values are typed in a serial terminal (type help), and every message follows the contract,
//   so tools/serial_capture.py, tools/contract_check.py and M2 read this board like any node.
#include <ArduinoJson.h>
#include <stdio.h>

#include "aeroion_contract.h"
#include "bench_board.h"
#include "bench_console.h"
#include "fog_detector.h"
#include "safety_core.h"
#include "telemetry_json.h"

namespace {

const NodeIdentity kIdentity = {"n90", "0.1.0-mcx-bench", 0.0f, 0.0f};  // n90: the bench, not a field node
const uint32_t kSafetyPeriodMs = 50;
const uint32_t kButtonPeriodMs = 10;
const uint32_t kTelemetryPeriodMs = 1000;
const uint32_t kStatusPeriodMs = 10000;
const uint32_t kWatchdogMs = 3000;
const float kRequestedLevel = 0.60f;  // what the controller (M6) will ask for

uint32_t seq = 0;
char lineBuffer[1024];

// Unix time estimated from the build time: right range, not exact (time_synced is false).
uint64_t nodeTimeMs() { return AEROION_BUILD_EPOCH_S * 1000ULL + bench_millis(); }

void printMessage(const char* kind, bool retain, JsonDocument& payload) {
  char topic[48];
  snprintf(topic, sizeof topic, AEROION_TOPIC_FMT, kIdentity.nodeId, kind);
  if (writeLine(lineBuffer, sizeof lineBuffer, topic, retain, payload.as<JsonObjectConst>()) > 0) {
    bench_write(lineBuffer);
    bench_write("\r\n");
  } else {
    bench_write("# ERROR: message larger than lineBuffer\r\n");
  }
}

class UartEventSink : public EventSink {
 public:
  void emit(const NodeEvent& event) override {
    JsonDocument payload;
    buildEventPayload(payload.to<JsonObject>(), kIdentity, nodeTimeMs(), ++seq, event);
    printMessage(AEROION_KIND_EVENT, false, payload);
  }
};

// A push button sampled every kButtonPeriodMs. update() is true once per press, after 3 equal samples.
class Button {
 public:
  Button() : raw_(false), stable_(false), count_(0) {}
  bool update(bool down) {
    count_ = (down == raw_) ? (count_ < 3 ? count_ + 1 : 3) : 0;
    raw_ = down;
    if (count_ == 3 && raw_ != stable_) {
      stable_ = raw_;
      return stable_;
    }
    return false;
  }

 private:
  bool raw_;
  bool stable_;
  int count_;
};

// LOCKED: short blue flash every 2 s | ARMED: green | HARVEST: blue (HV on) | BACKOFF: blue blinking
// SAFE: red | MAINTENANCE: yellow (red + green)
void showState(aeroion_state_t state, uint32_t nowMs) {
  bool flash = nowMs % 2000 < 80;
  bool blink = nowMs / 125 % 2 == 0;
  switch (state) {
    case AEROION_STATE_LOCKED: bench_led(false, false, flash); break;
    case AEROION_STATE_ARMED: bench_led(false, true, false); break;
    case AEROION_STATE_HARVEST: bench_led(false, false, true); break;
    case AEROION_STATE_BACKOFF: bench_led(false, false, blink); break;
    case AEROION_STATE_SAFE: bench_led(true, false, false); break;
    default: bench_led(true, true, false); break;  // MAINTENANCE
  }
}

void sendInfo() {
  const char* sensors[] = {"BENCH-CONSOLE", "SW2", "SW3"};
  JsonDocument payload;
  buildInfoPayload(payload.to<JsonObject>(), kIdentity, nodeTimeMs(), sensors, 3);
  printMessage(AEROION_KIND_INFO, true, payload);
}

void sendStatus() {
  JsonDocument payload;
  buildStatusPayload(payload.to<JsonObject>(), kIdentity, nodeTimeMs(), bench_millis() / 1000, false);
  printMessage(AEROION_KIND_STATUS, true, payload);
}

void reportRestart(EventSink& events) {
  switch (bench_restart_cause()) {
    case BENCH_RESTART_WATCHDOG:
      events.emit(NodeEvent::faultEvent(AEROION_FAULT_WATCHDOG, "restarted by the watchdog"));
      break;
    case BENCH_RESTART_CRASH:
      events.emit(NodeEvent::faultEvent(AEROION_FAULT_OTHER, "restarted after a crash"));
      break;
    case BENCH_RESTART_BROWNOUT:
      events.emit(NodeEvent::faultEvent(AEROION_FAULT_OTHER, "restarted after a brown-out (supply voltage too low)"));
      break;
    default:
      break;
  }
}

}  // namespace

int main(void) {
  bench_board_init();
  bench_write("\r\n# AeroIon-Edge M3 test bench on FRDM-MCXA156, contract " AEROION_SCHEMA_VERSION "\r\n");

  // Everything lives here rather than as globals, so nothing depends on C++ static initialisation.
  UartEventSink events;
  SafetyConfig config;  // defaults: firmware/node/lib/safety/safety_config.h
  FogDetector fogDetector(config, events);
  SafetyCore safety(config, events);
  BenchInputs inputs;
  BenchConsole console(safety, inputs, bench_write);
  NodeTelemetry telemetry;
  Button sw2;
  Button sw3;

  sendInfo();
  sendStatus();
  reportRestart(events);
  bench_write("# SW2 = operator badge, SW3 = technician badge. Type help for the commands.\r\n");
  if (!bench_watchdog_begin(kWatchdogMs)) bench_write("# WARNING: watchdog clock missing, watchdog OFF\r\n");

  uint32_t lastSafety = 0, lastButtons = 0, lastTelemetry = 0, lastStatus = 0;
  for (;;) {
    bench_watchdog_feed();
    uint32_t now = bench_millis();
    for (int c = bench_read(); c >= 0; c = bench_read()) console.feed(c, now);

    if (now - lastButtons >= kButtonPeriodMs) {
      lastButtons = now;
      if (sw2.update(bench_sw2_down())) console.tapOperator();
      if (sw3.update(bench_sw3_down())) console.tapTechnician();
    }

    if (now - lastSafety >= kSafetyPeriodMs) {
      lastSafety = now;
      SensorFrame& frame = telemetry.sensors;
      frame.clear();
      frame.fogIdxOpt = inputs.fogIdx;
      frame.radarPresence = inputs.personNear;
      frame.lidClosed = inputs.lidClosed;
      frame.currentUa = inputs.currentUa;
      fogDetector.update(frame.fogIdxOpt, AEROION_FOG_SOURCE_OPTICAL, safety.prearmed(now), now);

      SafetyInputs in;
      in.fogPresent = fogDetector.present();
      in.radarPresence = frame.radarPresence;
      in.lidClosed = frame.lidClosed;
      in.currentUa = frame.currentUa;
      in.arcRisk = inputs.arcRisk;
      safety.update(in, now);

      telemetry.state = safety.state();
      telemetry.hvOn = safety.hvAllowed();
      telemetry.hvLevel = safety.limitLevel(kRequestedLevel, now);
      telemetry.arcRisk = inputs.arcRisk;
    }
    showState(safety.state(), now);

    if (now - lastTelemetry >= kTelemetryPeriodMs) {
      lastTelemetry = now;
      JsonDocument payload;
      buildTelemetryPayload(payload.to<JsonObject>(), kIdentity, nodeTimeMs(), ++seq, telemetry);
      printMessage(AEROION_KIND_TELEMETRY, false, payload);
    }
    if (now - lastStatus >= kStatusPeriodMs) {
      lastStatus = now;
      sendStatus();
    }
  }
}