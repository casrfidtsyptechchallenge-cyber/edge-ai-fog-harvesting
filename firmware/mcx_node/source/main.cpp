// AeroIon-Edge node firmware for the NXP MCX A156, the node's safety controller (FRDM-MCXA156 board).
//
// M1 (sensing): reads the sensors wired to this chip (collector current, fog, HV power, radar, lid) through
//   the portable drivers in firmware/node/lib/drivers, and sends contract messages, one JSON line each:
//   info at boot, status every 10 s, telemetry every second. Lines for humans start with '#'.
// M3 (safety): every 50 ms the safety core decides whether the high voltage may be on, and how high; the RGB
//   LED shows its state and its events (STATE_CHANGE, FAULT, BADGE_TAP...) go out as they happen.
// M4 (RFID): cards on the RC522 reader are looked up in the node's badge list and go to the safety core.
// M7 (link): the JSON lines go out on two serial ports, the USB port to the PC and the UART to the node's
//   ESP32 (D0/D1), which publishes them on MQTT. Both bring the station's commands and the time back in;
//   tools/serial_bridge.py on the PC does the ESP32's job when there is no ESP32 (docs/link.md).
// M5a (arc predictor): while the HV is allowed, a 25.6 ms window of collector current at 10 kHz every 100 ms
//   goes through the 8-bit CNN (or its rule-based fallback); its risk goes to the safety core, which backs off
//   above ARC_WARN_RISK. A short arc seen in a window counts as an arc. Windows go out as "wave" messages on
//   a warning or an arc, and on demand (training data, docs/edge_ai.md).
// Next on this chip: the HV output.
#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aeroion_contract.h"
#include "arc_guard.h"
#include "arc_novelty.h"
#include "badge_list.h"
#include "bench_board.h"
#include "command_handler.h"
#include "fog_detector.h"
#include "line_reader.h"
#include "mcx_io.h"
#include "mcx_node_config.h"
#include "mcx_uart.h"
#include "node_console.h"
#include "node_link.h"
#include "node_sensors.h"
#include "safety_core.h"
#include "sensor_math.h"
#include "sensor_registry.h"
#include "telemetry_json.h"
#include "unix_clock.h"

namespace {

const NodeIdentity kIdentity = {NODE_ID, NODE_FW_VERSION, NODE_POS_X_M, NODE_POS_Y_M};
const uint32_t kButtonPeriodMs = 10;

struct LocalBadge {
  const char* uid;
  aeroion_role_t role;
};
const LocalBadge kLocalBadges[] = {AEROION_LOCAL_BADGES};  // mcx_node_config.h

uint32_t seq = 0;
char lineBuffer[1024];             // one message going out
char usbLine[LINK_LINE_MAX + 1];   // lines coming in from the PC
char linkLine[LINK_LINE_MAX + 1];  // lines coming in from the ESP32
char waveLine[3072];               // a wave message: 256 samples, about 2 kB
const char* pendingWave = nullptr; // the trigger of the next wave message: the safety core warned or saw an arc

// Text for people: the USB port only. The ESP32 gets the JSON lines, nothing else.
void usbPrint(const char* text) { mcx_uart_write(MCX_UART_USB, text); }

// A contract message: one JSON line, to the PC and to the ESP32.
void printMessage(const char* kind, bool retain, JsonDocument& payload) {
  char topic[48];
  snprintf(topic, sizeof topic, AEROION_TOPIC_FMT, kIdentity.nodeId, kind);
  if (writeLine(lineBuffer, sizeof lineBuffer, topic, retain, payload.as<JsonObjectConst>()) > 0) {
    for (int port = MCX_UART_USB; port <= MCX_UART_LINK; ++port) {
      mcx_uart_write(static_cast<mcx_uart_t>(port), lineBuffer);
      mcx_uart_write(static_cast<mcx_uart_t>(port), "\r\n");
    }
  } else {
    usbPrint("# ERROR: message larger than lineBuffer\r\n");
  }
}

// Events from every module go out at once, numbered with the same seq as the telemetry.
class UartEventSink : public EventSink {
 public:
  explicit UartEventSink(UnixClock& clock) : clock_(clock) {}
  void emit(const NodeEvent& event) override {
    JsonDocument payload;
    buildEventPayload(payload.to<JsonObject>(), kIdentity, clock_.now(bench_millis()), ++seq, event);
    printMessage(AEROION_KIND_EVENT, false, payload);
    if (event.type == AEROION_EVENT_ARC_PREDICTED) pendingWave = "ARC_PREDICTED";
    if (event.type == AEROION_EVENT_ARC) pendingWave = "ARC";
  }

 private:
  UnixClock& clock_;
};

SafetyConfig makeSafetyConfig() {
  SafetyConfig config;  // defaults: firmware/node/lib/safety/safety_config.h
  config.arcCurrentUa = SAFETY_ARC_CURRENT_UA;
  config.arcWarnRisk = ARC_WARN_RISK;
  config.requireRadar = AEROION_HAS_RADAR;
  config.requireLid = AEROION_HAS_LID_SWITCH;
  config.requireCurrent = AEROION_HAS_COLLECTOR_CURRENT;
  config.minCurrentUa = (AEROION_HAS_HV_OUTPUT && AEROION_HAS_COLLECTOR_CURRENT) ? SAFETY_MIN_CURRENT_UA : 0.0f;
  return config;
}

// LOCKED: short blue flash every 2 s | ARMED: green | HARVEST: blue (HV allowed) | BACKOFF: blue blinking
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

void sendInfo(const SensorRegistry& registry, UnixClock& clock) {
  const char* names[SensorRegistry::kMaxSensors];
  int count = registry.count();
  for (int i = 0; i < count; ++i) names[i] = registry.sensorAt(i)->name();
  JsonDocument payload;
  buildInfoPayload(payload.to<JsonObject>(), kIdentity, clock.now(bench_millis()), names, count);
  printMessage(AEROION_KIND_INFO, true, payload);
}

void sendStatus(UnixClock& clock) {
  uint32_t now = bench_millis();
  JsonDocument payload;
  buildStatusPayload(payload.to<JsonObject>(), kIdentity, clock.now(now), now / 1000, clock.synced(now));
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

// Prints every sensor's state at boot, then only the changes (e.g. "# sensor INA219 ok -> stale").
void reportSensorStates(const SensorRegistry& registry, SensorState* last, uint32_t nowMs, bool all) {
  char text[64];
  for (int i = 0; i < registry.count(); ++i) {
    SensorState state = registry.stateAt(i, nowMs);
    if (all) {
      snprintf(text, sizeof text, "# sensor %-12s %s\r\n", registry.sensorAt(i)->name(), sensorStateName(state));
      usbPrint(text);
    } else if (state != last[i]) {
      snprintf(text, sizeof text, "# sensor %-12s %s -> %s\r\n", registry.sensorAt(i)->name(),
               sensorStateName(last[i]), sensorStateName(state));
      usbPrint(text);
    }
    last[i] = state;
  }
}

// One JSON line from the PC or the ESP32: a command or the time (lib/link/node_link.h).
void handleLinkLine(NodeLink& link, const CommandHandler& commands, const char* line, uint32_t nowMs) {
  char text[160];
  switch (link.handleLine(line, nowMs)) {
    case NodeLink::kCommand:
      if (commands.ackSent()) {
        snprintf(text, sizeof text, "# station command %s: %s%s%s\r\n", commands.lastType(),
                 aeroion_ack_name(commands.lastResult()), commands.lastReason()[0] ? ", " : "",
                 commands.lastReason());
      } else {
        snprintf(text, sizeof text, "# station command %s ignored: %s\r\n", commands.lastType(),
                 commands.lastReason());
      }
      usbPrint(text);
      break;
    case NodeLink::kBadJson:
      snprintf(text, sizeof text, "# not a JSON line (%s): ignored\r\n", link.lastError());
      usbPrint(text);
      break;
    case NodeLink::kUnknown:
      usbPrint("# JSON line that is not a command or the time: ignored\r\n");
      break;
    default:  // the time, or a command for another node
      break;
  }
}

// ---------------------------------------------------------------- arc predictor (M5a)

// Windows of collector current from the fast ADC, in µA (the same zero and gain as the 10 Hz reading).
class McxCurrentWindow : public ArcWindowSource {
 public:
  explicit McxCurrentWindow(NodeSensors& sensors) : sensors_(sensors) {}
  float capture(float* ua, int n, float fsHz) override {
#if AEROION_HAS_COLLECTOR_CURRENT
    uint32_t spanUs = 0;
    if (!mcx_io_burst_mv(PIN_COLLECTOR_I, ua, n, static_cast<uint32_t>(1.0e6f / fsHz + 0.5f), &spanUs) || spanUs == 0) {
      return 0.0f;
    }
    float zero = sensors_.collectorCurrent.zeroMv();
    for (int i = 0; i < n; ++i) {
      ua[i] = sensor_math::collectorMicroamps(ua[i], zero, COLLECTOR_AMP_GAIN, COLLECTOR_SHUNT_OHMS);
    }
    return (n - 1) * 1.0e6f / spanUs;
#else
    (void)ua;
    (void)n;
    (void)fsHz;
    return 0.0f;  // no collector current sensor: no arc predictor
#endif
  }

 private:
  NodeSensors& sensors_;
};

// A number with a few decimals, without %f (small C libraries lack it).
const char* fixed(char* out, size_t cap, float value, int decimals) {
  if (!(value == value)) {  // NaN
    snprintf(out, cap, "-");
    return out;
  }
  long scale = decimals == 0 ? 1 : (decimals == 1 ? 10 : (decimals == 2 ? 100 : 1000));
  long v = lroundf(value * scale);
  unsigned long mag = static_cast<unsigned long>(v < 0 ? -v : v);
  if (decimals == 0) {
    snprintf(out, cap, "%s%lu", v < 0 ? "-" : "", mag);
  } else {
    snprintf(out, cap, "%s%lu.%0*lu", v < 0 ? "-" : "", mag / scale, decimals, mag % scale);
  }
  return out;
}

void sendWave(ArcGuard& guard, UnixClock& clock, const char* trigger) {
  size_t len = formatWaveLine(waveLine, sizeof waveLine, kIdentity.nodeId, AEROION_SCHEMA_VERSION,
                              clock.now(guard.windowMs()), ++seq, trigger, static_cast<int>(guard.fsHz() + 0.5f),
                              guard.window(), guard.length());
  if (len == 0) {
    usbPrint("# ERROR: wave message larger than waveLine\r\n");
    return;
  }
  for (int port = MCX_UART_USB; port <= MCX_UART_LINK; ++port) {
    mcx_uart_write(static_cast<mcx_uart_t>(port), waveLine);
    mcx_uart_write(static_cast<mcx_uart_t>(port), "\r\n");
  }
}

void printArcStatus(ArcGuard& guard, bool hvAllowed) {
  char text[200], a[16], b[16], c[16], d[16], e[16];
  ArcPredictor& p = guard.predictor();
  snprintf(text, sizeof text, "# arc predictor (M5a): model %s, mode %s, warns above %s\r\n", ArcPredictor::modelName(),
           ArcPredictor::modeName(p.mode()), fixed(a, sizeof a, ARC_WARN_RISK, 2));
  usbPrint(text);
  if (guard.windows() == 0) {
    usbPrint(hvAllowed ? "# no window yet\r\n" : "# no window yet: windows are taken while the HV is allowed\r\n");
    return;
  }
  const ArcWindowInfo& w = guard.info();
  snprintf(text, sizeof text, "# last window: risk %s (%s)%s, rule %s; median %s uA, peak %s uA, %s pulses/s\r\n",
           fixed(a, sizeof a, guard.risk().getOr(NAN), 2), w.usedModel ? "CNN" : "rule",
           hvAllowed ? "" : " [HV now off]", fixed(b, sizeof b, w.ruleRisk, 2), fixed(c, sizeof c, w.medianUa, 1),
           fixed(d, sizeof d, w.peakUa, 1), fixed(e, sizeof e, w.pulsesPerS, 0));
  usbPrint(text);
  snprintf(text, sizeof text, "# novelty guard: last window %s, %s%% of recent windows novel, %lu in all\r\n",
           ArcNovelty::reasonName(static_cast<ArcNovelty::Reason>(w.novelty)),
           guard.novelShare().has() ? fixed(a, sizeof a, 100.0f * guard.novelShare().get(), 0) : "-",
           static_cast<unsigned long>(guard.novelWindows()));
  usbPrint(text);
  snprintf(text, sizeof text, "# %d samples at %s Hz in %lu us, judged in %lu us; %lu windows, %lu failed\r\n",
           guard.length(), fixed(a, sizeof a, guard.fsHz(), 0), static_cast<unsigned long>(guard.captureUs()),
           static_cast<unsigned long>(guard.inferenceUs()), static_cast<unsigned long>(guard.windows()),
           static_cast<unsigned long>(guard.failures()));
  usbPrint(text);
}

// The arc predictor's console commands. False: not one of them (the node console takes the line).
bool arcCommand(const char* line, ArcGuard& guard, bool hvAllowed, bool* waveNow, uint32_t* waveEveryMs,
                bool* observe) {
  char command[16] = "", arg[16] = "";
  if (sscanf(line, "%15s %15s", command, arg) < 1) return false;
  if (strcmp(command, "ai") == 0) {
    // ai observe: the predictor runs and its risk goes out in the telemetry, but the safety core does not back
    // off on it (the 250 µA arc detection and its back-off stay on). For training-data campaigns in the chamber,
    // with an operator watching: what follows each window is then not changed by the model (no intervention
    // bias, docs/datasets.md). ai act: back to normal.
    if (strcmp(arg, "observe") == 0) *observe = true;
    if (strcmp(arg, "act") == 0) *observe = false;
    if (*observe) usbPrint("# ai OBSERVE: predictions are logged, NOT acted on (arc detection still on)\r\n");
    if (strcmp(arg, "auto") == 0) guard.predictor().setMode(ArcPredictor::kAuto);
    if (strcmp(arg, "model") == 0) guard.predictor().setMode(ArcPredictor::kModel);
    if (strcmp(arg, "rules") == 0) guard.predictor().setMode(ArcPredictor::kRules);
    printArcStatus(guard, hvAllowed);
  } else if (strcmp(command, "wave") == 0) {
    *waveNow = true;
    usbPrint(hvAllowed ? "# the next window goes out as a wave message (MANUAL)\r\n"
                       : "# a wave message goes out with the next window, once the HV is allowed\r\n");
  } else if (strcmp(command, "waves") == 0) {
    int seconds = strcmp(arg, "off") == 0 ? 0 : atoi(arg);
    *waveEveryMs = seconds > 0 ? static_cast<uint32_t>(seconds) * 1000U : 0;
    if (seconds > 0) {
      char text[96];
      snprintf(text, sizeof text, "# a wave message every %d s while the HV is allowed\r\n", seconds);
      usbPrint(text);
    } else {
      usbPrint("# periodic wave messages off\r\n");
    }
  } else {
    return false;
  }
  return true;
}

// Says so when a port loses received data: a line too long for the node, or bytes lost.
class PortWatch {
 public:
  PortWatch(const char* name, mcx_uart_t port, const LineReader& lines)
      : name_(name), port_(port), lines_(lines), dropped_(0), lost_(0) {}
  void check() {
    char text[96];
    if (lines_.dropped() != dropped_) {
      dropped_ = lines_.dropped();
      snprintf(text, sizeof text, "# %s: a line longer than %d characters was dropped\r\n", name_, LINK_LINE_MAX);
      usbPrint(text);
    }
    if (mcx_uart_lost(port_) != lost_) {
      lost_ = mcx_uart_lost(port_);
      snprintf(text, sizeof text, "# %s: %lu received bytes lost so far\r\n", name_, static_cast<unsigned long>(lost_));
      usbPrint(text);
    }
  }

 private:
  const char* name_;
  mcx_uart_t port_;
  const LineReader& lines_;
  uint32_t dropped_;
  uint32_t lost_;
};

}  // namespace

int main(void) {
  bench_board_init();
  mcx_uart_init(LINK_BAUD);  // from here on, serial output goes through mcx_uart (interrupts)
  mcx_io_init(ADC_FULL_SCALE_MV);
  usbPrint("\r\n# AeroIon-Edge node " NODE_ID " on the MCX A156 (safety controller), firmware " NODE_FW_VERSION
           ", contract " AEROION_SCHEMA_VERSION "\r\n");

  // Everything lives here rather than as globals, so nothing depends on C++ static initialisation.
  UnixClock clock(AEROION_BUILD_EPOCH_S * 1000ULL);  // the build time, until the link sends the time
  UartEventSink events(clock);
  const SafetyConfig config = makeSafetyConfig();
  FogDetector fogDetector(config, events);
  SafetyCore safety(config, events);
  NodeSensors sensors;
  SensorRegistry registry;
  NodeTelemetry telemetry;  // fog_idx_virt and health are filled by the ESP32 (M5b, M5c)
  BadgeList badges;
  CommandHandler commands(safety, badges, events);
  NodeLink link(NODE_ID, clock, commands, events, LINK_SILENT_MS);
  NodeConsole console(safety, registry, sensors, badges, link, usbPrint);
  LineReader usbLines(usbLine, sizeof usbLine);
  LineReader linkLines(linkLine, sizeof linkLine);
  PortWatch usbWatch("USB", MCX_UART_USB, usbLines);
  PortWatch linkWatch("ESP32 link", MCX_UART_LINK, linkLines);
  SensorState sensorStates[SensorRegistry::kMaxSensors];
  McxCurrentWindow currentWindow(sensors);
  ArcGuard arcGuard(currentWindow, ARC_PERIOD_MS, bench_micros);
  bool waveNow = false;
  bool arcObserve = false;  // ai observe: training campaign, the safety core does not act on the predictor
  uint32_t waveEveryMs = ARC_WAVE_EVERY_S * 1000U;
  uint32_t lastWaveMs = 0;
  uint32_t lastNovelWaveMs = 0u - NOVEL_WAVE_GAP_MS;

  for (const LocalBadge& badge : kLocalBadges) {
    if (!badges.set(badge.uid, badge.role)) {
      usbPrint("# WARNING: a badge in mcx_node_config.h has an invalid UID\r\n");
    }
  }
  sensors.registerAll(registry);
  uint32_t now = bench_millis();
  registry.beginAll(now);  // the collector-current zero is measured here, with the HV off
  if (registry.count() == 0) usbPrint("# no sensors enabled in mcx_node_config.h\r\n");
  reportSensorStates(registry, sensorStates, now, true);
  if (!config.requireRadar) usbPrint("# WARNING: no radar installed: the person cut-off is OFF\r\n");
  if (!config.requireLid) usbPrint("# WARNING: no lid switch installed: the lid interlock is OFF\r\n");
  if (!config.requireCurrent) usbPrint("# WARNING: no collector current sensor: arcs are not detected\r\n");
  if (config.minCurrentUa <= 0.0f) usbPrint("# NOTE: current plausibility check off (no HV output yet)\r\n");

  sendInfo(registry, clock);
  sendStatus(clock);
  reportRestart(events);
  usbPrint("# SW2 = operator badge, SW3 = technician badge. Type help for the commands.\r\n");
  usbPrint("# Station link: the ESP32 on D0/D1, or tools/serial_bridge.py on this USB port (type link).\r\n");
  if (!bench_watchdog_begin(WATCHDOG_TIMEOUT_MS)) usbPrint("# WARNING: watchdog clock missing, watchdog OFF\r\n");

  uint32_t lastSafety = now - SAFETY_PERIOD_MS;  // the safety core runs first, before any badge is read
  uint32_t lastButtons = now, lastTelemetry = now, lastStatus = now;
  for (;;) {
    bench_watchdog_feed();
    now = bench_millis();
    registry.pollAll(now);

    // The safety loop: fresh sensor values in, HV permission and level out.
    if (now - lastSafety >= SAFETY_PERIOD_MS) {
      lastSafety = now;
      SensorFrame& frame = telemetry.sensors;
      registry.fillFrame(frame, now);
      fogDetector.update(frame.fogIdxOpt, AEROION_FOG_SOURCE_OPTICAL, safety.prearmed(now), now);

      // The arc predictor (M5a): a window every ARC_PERIOD_MS while the HV is allowed (25.6 ms of this loop).
      bool newWindow = arcGuard.poll(safety.hvAllowed(), now);
      if (newWindow && arcGuard.info().peakUa > config.arcCurrentUa) {
        safety.reportArc(arcGuard.info().peakUa, now);  // an arc too short for the 10 Hz reading
      }
      telemetry.arcRisk = arcGuard.risk();
      telemetry.arcNovel = arcGuard.novelShare();

      SafetyInputs in;
      in.fogPresent = fogDetector.present();
      in.radarPresence = frame.radarPresence;
      in.lidClosed = frame.lidClosed;
      in.currentUa = frame.currentUa;
      if (!arcObserve) in.arcRisk = arcGuard.risk();  // above ARC_WARN_RISK: ARC_PREDICTED and BACKOFF
      safety.update(in, now);

      // A window as a wave message: the one the safety core warned or saw an arc on, or asked for (training data)
      if (newWindow) {
        const char* trigger = pendingWave;
        if (trigger == nullptr && (waveNow || (waveEveryMs > 0 && now - lastWaveMs >= waveEveryMs))) {
          trigger = "MANUAL";
        }
        // A window unlike the training data: training material for M11, at most one every NOVEL_WAVE_GAP_MS
        // (a wave is 2 kB on the UART).
        if (trigger == nullptr && arcGuard.lastNovel() && now - lastNovelWaveMs >= NOVEL_WAVE_GAP_MS) {
          trigger = "NOVEL";
          lastNovelWaveMs = now;
        }
        if (trigger != nullptr) {
          sendWave(arcGuard, clock, trigger);
          lastWaveMs = now;
          waveNow = false;
        }
        pendingWave = nullptr;
      }
      if (!safety.hvAllowed()) pendingWave = nullptr;  // HV off (a fault): that arc's window is not coming

      telemetry.state = safety.state();
      telemetry.hvOn = safety.hvAllowed();  // no HV output on this chip yet: this says "allowed"
      // The station's SET_HINT while it is valid, else the default; the safety core has the last word.
      telemetry.hvLevel = safety.limitLevel(commands.requestedLevel(HV_DEFAULT_LEVEL, now), now);
    }
    showState(safety.state(), now);

    // From the PC: JSON lines are for the link, everything else is a console command.
    for (int c = mcx_uart_read(MCX_UART_USB); c >= 0; c = mcx_uart_read(MCX_UART_USB)) {
      char* line = usbLines.feed(c);
      if (line == nullptr) continue;
      if (line[0] == '{') {
        handleLinkLine(link, commands, line, now);
      } else if (!arcCommand(line, arcGuard, safety.hvAllowed(), &waveNow, &waveEveryMs, &arcObserve)) {
        console.run(line, now);
      }
    }
    // From the ESP32: JSON lines only.
    for (int c = mcx_uart_read(MCX_UART_LINK); c >= 0; c = mcx_uart_read(MCX_UART_LINK)) {
      char* line = linkLines.feed(c);
      if (line != nullptr && line[0] == '{') handleLinkLine(link, commands, line, now);
    }
    link.poll(now);  // FAULT COMM_LOST if the link went silent
    if (link.takeCameUp()) {
      usbPrint("# station link up: the clock is synced\r\n");
      sendInfo(registry, clock);  // the bridge or the ESP32 may have started after this node
      sendStatus(clock);
      lastStatus = now;
    }
    usbWatch.check();
    linkWatch.check();

    char uid[21];
    if (sensors.takeBadgeTap(uid, sizeof uid)) console.tapCard(uid);  // a card on the RFID reader (M4)
    if (now - lastButtons >= kButtonPeriodMs) {
      lastButtons = now;
      console.buttons(bench_sw2_down(), bench_sw3_down());
    }

    if (now - lastTelemetry >= TELEMETRY_PERIOD_MS) {
      lastTelemetry = now;
      JsonDocument payload;
      buildTelemetryPayload(payload.to<JsonObject>(), kIdentity, clock.now(bench_millis()), ++seq, telemetry);
      printMessage(AEROION_KIND_TELEMETRY, false, payload);
      reportSensorStates(registry, sensorStates, now, false);
    }
    if (now - lastStatus >= STATUS_PERIOD_MS) {
      lastStatus = now;
      sendStatus(clock);
    }
  }
}