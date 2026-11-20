// AeroIon-Edge: firmware of the ESP32-S3 in a two-chip node (architecture v2), M7 part 2.
//
// The MCX A156 is the safety controller (firmware/mcx_node): fast sensors, safety core, badges, HV. This
// chip is the node's radio and has the slow sensors:
//   - Wi-Fi and MQTT to the station: the MCX's messages go out through here, the station's commands come in;
//   - the UART to the MCX (GPIO 17/18 to the MCX's D0/D1), and the time (NTP) for the MCX's clock;
//   - the load cell (HX711) and the SHT45, merged into the MCX's telemetry; TARE is done here;
//   - an offline buffer while the broker cannot be reached;
//   - FOG_ALERT by radio (ESP-NOW broadcast) to and from the neighbouring nodes (part 3);
//   - the edge AI models that need only the 1 s telemetry (M5): electrode health and virtual fog sensor;
//   - the HV controller (M6): the level for the most water per Wh, asked from the MCX (it only asks).
// The decisions are in lib/gateway, lib/edge_ai and lib/controller (tested on the PC); this file joins them
// to the hardware.
// Build: pio run -e esp32s3-link (real board) or pio run -e esp32s3-link-wokwi (simulator). docs/link.md.
#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <sys/time.h>
#include <time.h>

#include "../watchdog.h"
#include "controller_hook.h"
#include "esp_models.h"
#include "line_reader.h"
#include "link_config.h"
#include "node_gateway.h"
#include "sensor_registry.h"
#if LINK_HAS_HX711
#include "../sensors/hx711_sensor.h"
#endif
#if LINK_HAS_SHT45
#include "../sensors/sht45_sensor.h"
#endif

#ifndef AEROION_BUILD_EPOCH_S
#define AEROION_BUILD_EPOCH_S 1767225600ULL  // 2026-01-01, used only if the build did not set it
#endif

namespace {

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
SensorRegistry registry;
#if LINK_HAS_HX711
Hx711Sensor loadCell(PIN_HX711_DOUT, PIN_HX711_SCK, HX711_COUNTS_PER_GRAM);
int loadCellIndex = -1;
#endif
#if LINK_HAS_SHT45
Sht45Sensor sht45(Wire);
#endif

const char* const kSensorNames[] = {
#if LINK_HAS_SHT45
    "SHT45",
#endif
#if LINK_HAS_HX711
    "HX711",
#endif
    nullptr};  // end of the list
const int kSensorCount = sizeof kSensorNames / sizeof kSensorNames[0] - 1;

char mcxLineBuffer[4096];  // the MCX's longest lines: a wave message (M5a), about 2 kB
LineReader mcxLines(mcxLineBuffer, sizeof mcxLineBuffer);
char consoleBuffer[64];
LineReader consoleLines(consoleBuffer, sizeof consoleBuffer);

const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t radioQueue = nullptr;  // packets from the radio task to loop()
bool radioReady = false;

uint32_t lastMqttTryMs = 0;
bool mqttTried = false;
wl_status_t lastWifiStatus = WL_IDLE_STATUS;
bool lastClockSynced = false;
uint32_t lastReadingsMs = 0;
uint32_t lastStatusPrintMs = 0;

EspModels models;        // M5b virtual fog, M5c electrode health: run on the MCX's telemetry (lib/edge_ai)
HvController controller;  // M6: the HV level for the most water per Wh, asked from the MCX (lib/controller)
ControllerHook controllerHook(controller);
HookChain hooks(models, controllerHook);  // the models first: the controller reads what they fill in
Preferences settings;    // the clean electrode's onset, kept in flash across restarts
float savedReference = NAN;
uint32_t lastReferenceCheckMs = 0;

void logLine(const char* text) {
  Serial.println(text);
#if LINK_LOG_TO_MCX_PORT
  LINK_SERIAL.println(text);  // Wokwi: the serial monitor is on the MCX's pins (the MCX ignores '#' lines)
#endif
}

// The gateway's view of this board.
class EspPort : public GatewayPort {
 public:
  bool mqttConnected() override { return mqtt.connected(); }
  bool publish(const char* topic, const char* payload, bool retain) override {
    return mqtt.publish(topic, payload, retain);
  }
  void toMcx(const char* line) override {
    LINK_SERIAL.print(line);
    LINK_SERIAL.print('\n');
  }
  void log(const char* text) override { logLine(text); }
  bool radioSend(const uint8_t* data, size_t length) override {
    return radioReady && esp_now_send(kBroadcast, data, length) == ESP_OK;
  }
};

GatewayConfig makeConfig() {
  GatewayConfig config;  // timings: lib/gateway/node_gateway.h
  config.nodeId = LINK_NODE_ID;
  config.fwVersion = LINK_FW_VERSION;
  config.sensorNames = kSensorNames;
  config.sensorCount = kSensorCount;
  config.radioOnLink = LINK_RADIO_ON_LINK;
  return config;
}

EspPort port;
NodeGateway gateway(makeConfig(), port);

// ---------------------------------------------------------------- time

// NTP has set the clock (a date from 2024 on). Before that, timestamps count from the build time.
bool clockSynced() { return time(nullptr) > 1704067200; }

uint64_t unixMs() {
  if (clockSynced()) {
    struct timeval now;
    gettimeofday(&now, nullptr);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL + static_cast<uint64_t>(now.tv_usec) / 1000ULL;
  }
  return static_cast<uint64_t>(AEROION_BUILD_EPOCH_S) * 1000ULL + static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

// ---------------------------------------------------------------- load cell

bool tareLoadCell() {
#if LINK_HAS_HX711
  if (loadCellIndex < 0 || registry.stateAt(loadCellIndex, millis()) != SensorState::Ok) return false;
  loadCell.tare();
  return true;
#else
  return false;
#endif
}

// ---------------------------------------------------------------- Wi-Fi and MQTT

void onMessage(char* topic, uint8_t* payload, unsigned int length) {
  gateway.fromBroker(topic, reinterpret_cast<const char*>(payload), length, unixMs(), clockSynced(), millis());
}

void watchWifi() {
  wl_status_t status = WiFi.status();
  if (status == lastWifiStatus) return;
  char text[96];
  if (status == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(text, sizeof text, "# Wi-Fi connected to %s, IP %u.%u.%u.%u, signal %d dBm", WIFI_SSID, ip[0], ip[1],
             ip[2], ip[3], static_cast<int>(WiFi.RSSI()));
  } else if (lastWifiStatus == WL_CONNECTED) {
    snprintf(text, sizeof text, "# Wi-Fi lost, reconnecting");
  } else {
    snprintf(text, sizeof text, "# Wi-Fi: looking for %s (status %d)", WIFI_SSID, static_cast<int>(status));
  }
  logLine(text);
  lastWifiStatus = status;
}

void keepMqtt(uint32_t nowMs) {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (mqttTried && nowMs - lastMqttTryMs < MQTT_RETRY_MS) return;
  mqttTried = true;
  lastMqttTryMs = nowMs;
  watchdogFeed();  // connecting can take a few seconds
  char will[96];
  gateway.lastWill(will, sizeof will);
  const char* clientId = "aeroion-" LINK_NODE_ID;
  bool ok = MQTT_USER[0] == '\0'
                ? mqtt.connect(clientId, gateway.statusTopic(), 1, true, will)
                : mqtt.connect(clientId, MQTT_USER, MQTT_PASSWORD, gateway.statusTopic(), 1, true, will);
  char text[128];
  if (ok) {
    mqtt.subscribe(gateway.cmdTopic(), 1);
    mqtt.subscribe(AEROION_TOPIC_BROADCAST_CMD, 1);
    snprintf(text, sizeof text, "# MQTT connected to %s:%d, listening to %s and %s", MQTT_HOST, MQTT_PORT,
             gateway.cmdTopic(), AEROION_TOPIC_BROADCAST_CMD);
  } else {
    snprintf(text, sizeof text, "# MQTT: no answer from %s:%d (state %d), trying again in %d s", MQTT_HOST, MQTT_PORT,
             mqtt.state(), MQTT_RETRY_MS / 1000);
  }
  logLine(text);
}

// ---------------------------------------------------------------- radio (ESP-NOW)

// Runs in the Wi-Fi task: only copy the packet; loop() hands it to the gateway.
#if ESP_IDF_VERSION_MAJOR >= 5
void onRadio(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
  (void)info;
#else
void onRadio(const uint8_t* mac, const uint8_t* data, int length) {
  (void)mac;
#endif
  if (length == static_cast<int>(kFogAlertSize) && radioQueue != nullptr) xQueueSend(radioQueue, data, 0);
}

// ESP-NOW on the Wi-Fi channel of the access point: every node on the same network hears the broadcasts.
void startRadio() {
  radioQueue = xQueueCreate(8, kFogAlertSize);
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof peer);
  memcpy(peer.peer_addr, kBroadcast, sizeof kBroadcast);
  peer.channel = 0;  // the current channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  radioReady = radioQueue != nullptr && esp_now_init() == ESP_OK && esp_now_register_recv_cb(onRadio) == ESP_OK &&
               esp_now_add_peer(&peer) == ESP_OK;
  logLine(radioReady ? "# radio (ESP-NOW) ready: FOG_ALERT to and from the neighbours"
                     : "# radio (ESP-NOW) failed to start: no FOG_ALERT by radio");
}

// ---------------------------------------------------------------- edge AI (M5b, M5c)

// The electrode's clean onset (the health score's reference) survives a restart; cleaning forgets it.
void keepHealthReference() {
  if (models.takeReferenceCleared()) {
    settings.remove("onset_ref");
    savedReference = NAN;
    logLine("# electrode health: the electrode was serviced, learning its clean onset again");
  }
  Nullable<float> ref = models.health().reference();
  if (ref.has() && !(fabsf(ref.get() - savedReference) < 1e-4f)) {
    settings.putFloat("onset_ref", ref.get());
    savedReference = ref.get();
    char text[96];
    snprintf(text, sizeof text, "# electrode health: clean onset %.3f saved (the score compares with it)", ref.get());
    logLine(text);
  }
}

void printHealth() {
  char text[160];
  ElectrodeHealth& h = models.health();
  Nullable<float> onset = h.onset(), k = h.kCoef(), score = h.score(), ref = h.reference();
  snprintf(text, sizeof text, "# electrode health (M5c): %d fit(s), onset %.3f, K %.1f, score %.2f, clean onset %.3f",
           h.fits(), onset.getOr(NAN), k.getOr(NAN), score.getOr(NAN), ref.getOr(NAN));
  logLine(text);
  VirtualFog& f = models.fog();
  Nullable<float> fog = models.lastFog();
  snprintf(text, sizeof text, "# virtual fog (M5b): %s, K clear %.1f, fog lowers it by %.0f %% at most, last %.2f",
           f.calibrated() ? "calibrated" : "learning from the optical sensor", f.kClear(), 100.0f * f.suppression(),
           fog.getOr(NAN));
  logLine(text);
  logLine("# (nan = not known yet: the models need the HV on and the level moving; type clean after a cleaning)");
}

// ---------------------------------------------------------------- HV controller (M6)

void printController() {
  char text[200];
  const HvController& c = controller;
  bool efficiency = c.goal() == ControllerGoal::Efficiency;
  snprintf(text, sizeof text, "# HV controller (M6): %s, goal %s; asks %.2f, ceiling %.2f; last result %.2f %s",
           c.enabled() ? "on" : "OFF (the MCX uses its default level)", efficiency ? "grams per Wh" : "water",
           c.target().getOr(NAN), c.ceiling(), c.objective().getOr(NAN),
           efficiency ? "g/Wh per unit of fog" : "g/h per unit of fog");
  logLine(text);
  snprintf(text, sizeof text, "# last move: %s; %lu move(s), %lu arc(s) or warning(s); %lu request(s) to the MCX, "
           "%lu accepted, %lu refused",
           c.lastReason(), static_cast<unsigned long>(c.moves()), static_cast<unsigned long>(c.arcs()),
           static_cast<unsigned long>(gateway.levelRequests()), static_cast<unsigned long>(controllerHook.accepted()),
           static_cast<unsigned long>(controllerHook.rejected()));
  logLine(text);
  if (c.stationHintActive(millis())) logLine("# a SET_HINT from the station is in force: M6 waits until it expires");
}

void controllerCommand(const char* arg) {
  if (strcmp(arg, "on") == 0) {
    controller.setEnabled(true);
  } else if (strcmp(arg, "off") == 0) {
    controller.setEnabled(false);
  } else if (strcmp(arg, "water") == 0) {
    controller.setGoal(ControllerGoal::Water);
  } else if (strcmp(arg, "efficiency") == 0) {
    controller.setGoal(ControllerGoal::Efficiency);
  } else if (arg[0] != '\0') {
    logLine("# m6 commands: m6 | m6 on | m6 off | m6 efficiency | m6 water");
    return;
  }
  printController();
}

// ---------------------------------------------------------------- console (USB serial port)

void printStatus() {
  char text[160];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(text, sizeof text, "# status: Wi-Fi %d dBm, MQTT %s, MCX %s, clock %s", static_cast<int>(WiFi.RSSI()),
             mqtt.connected() ? "connected" : "NOT connected", gateway.mcxAlive() ? "answering" : "SILENT",
             clockSynced() ? "synced" : "NOT synced");
  } else {
    snprintf(text, sizeof text, "# status: Wi-Fi NOT connected, MCX %s", gateway.mcxAlive() ? "answering" : "SILENT");
  }
  logLine(text);
  snprintf(text, sizeof text, "# messages: %lu published, %lu commands to the MCX, %d kept offline, %lu lost",
           static_cast<unsigned long>(gateway.published()), static_cast<unsigned long>(gateway.forwarded()),
           gateway.buffered(), static_cast<unsigned long>(gateway.dropped()));
  logLine(text);
  snprintf(text, sizeof text, "# FOG_ALERT: %lu sent, %lu heard from neighbours, radio %s",
           static_cast<unsigned long>(gateway.alertsSent()), static_cast<unsigned long>(gateway.alertsHeard()),
           radioReady ? "ready" : "OFF");
  logLine(text);
}

void runConsole(char* line) {
  if (strcmp(line, "status") == 0) {
    printStatus();
  } else if (strcmp(line, "tare") == 0) {
    logLine(tareLoadCell() ? "# load cell: zero measured again" : "# load cell: not installed or not answering");
  } else if (strcmp(line, "health") == 0) {
    printHealth();
  } else if (strcmp(line, "clean") == 0) {
    models.clean();
    keepHealthReference();
  } else if (strncmp(line, "m6", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
    controllerCommand(line[2] == ' ' ? line + 3 : "");
  } else {
    logLine("# commands: status | tare | health | clean (the electrode was cleaned) | m6 [on|off|efficiency|water]");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  LINK_SERIAL.setRxBufferSize(8192);  // keeps the MCX's lines while a connection attempt blocks
  LINK_SERIAL.setTxBufferSize(4096);
  LINK_SERIAL.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);
  delay(500);
  logLine("# AeroIon-Edge node " LINK_NODE_ID ", ESP32-S3 radio of a two-chip node, firmware " LINK_FW_VERSION
          ", contract " AEROION_SCHEMA_VERSION);

#if LINK_HAS_SHT45
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  registry.add(&sht45);
#endif
#if LINK_HAS_HX711
  loadCellIndex = registry.count();
  registry.add(&loadCell);
#endif
  registry.beginAll(millis());
  gateway.setTare(tareLoadCell);
  gateway.setHook(&hooks);
  settings.begin("aeroion", false);
  savedReference = settings.getFloat("onset_ref", NAN);
  if (isfinite(savedReference)) models.health().setReference(savedReference);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(MQTT_BUFFER_BYTES);
  mqtt.setSocketTimeout(3);  // seconds to wait for the broker's answers (the default is 15)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // keep the radio listening: in power save, the neighbours' broadcasts are often missed
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  configTime(0, 0, NTP_SERVER, MQTT_HOST);  // UTC; the second server: the station's PC or Pi, if it serves NTP
  startRadio();  // after WiFi.mode(): ESP-NOW needs the Wi-Fi driver running

  aeroion_fault_t code;
  const char* detail;
  if (abnormalRestart(&code, &detail)) gateway.emit(NodeEvent::faultEvent(code, detail), unixMs());
  logLine("# type status for a summary");
  watchdogBegin(LINK_WATCHDOG_MS);
}

void loop() {
  watchdogFeed();
  uint32_t now = millis();
  watchWifi();
  keepMqtt(now);
  mqtt.loop();  // the station's commands arrive here (onMessage)

  while (LINK_SERIAL.available() > 0) {
    char* line = mcxLines.feed(LINK_SERIAL.read());
    if (line != nullptr) gateway.fromMcx(line, unixMs(), millis());
  }
  uint8_t packet[kFogAlertSize];
  while (radioQueue != nullptr && xQueueReceive(radioQueue, packet, 0) == pdTRUE) {
    gateway.fromRadio(packet, sizeof packet, unixMs(), clockSynced(), millis());
  }
  while (Serial.available() > 0) {
    char* line = consoleLines.feed(Serial.read());
    if (line != nullptr) runConsole(line);
  }

  registry.pollAll(now);
  if (now - lastReadingsMs >= READINGS_PERIOD_MS) {
    lastReadingsMs = now;
    SensorFrame frame;
    registry.fillFrame(frame, now);
    EspReadings readings;
    readings.tempC = frame.tempC;
    readings.rhPct = frame.rhPct;
    readings.massG = frame.massG;
    readings.rateGpm = frame.rateGpm;
    if (WiFi.status() == WL_CONNECTED) readings.rssiDbm = static_cast<float>(WiFi.RSSI());
    gateway.setReadings(readings);
  }

  bool synced = clockSynced();
  if (synced && !lastClockSynced) logLine("# clock synced (NTP): the MCX gets the time every 5 s");
  lastClockSynced = synced;
  gateway.poll(unixMs(), synced, now);

  if (now - lastReferenceCheckMs >= 60000) {
    lastReferenceCheckMs = now;
    keepHealthReference();
  }
  if (now - lastStatusPrintMs >= STATUS_PRINT_MS) {
    lastStatusPrintMs = now;
    printStatus();
  }
  delay(1);
}