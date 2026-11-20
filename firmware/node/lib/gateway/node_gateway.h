#pragma once
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "fog_alert.h"
#include "node_event.h"
#include "nullable.h"
#include "offline_buffer.h"

// What the gateway needs from the ESP32's hardware (src/link/main_link.cpp). The tests use a fake one.
class GatewayPort {
 public:
  virtual ~GatewayPort() {}
  virtual bool mqttConnected() = 0;
  virtual bool publish(const char* topic, const char* payload, bool retain) = 0;  // false: not sent
  virtual void toMcx(const char* line) = 0;  // one line to the MCX; the port adds the line end
  virtual void log(const char* text) = 0;    // one line for people, starting with '#'
  // A radio broadcast to the neighbours (ESP-NOW). False: not sent (no radio here).
  virtual bool radioSend(const uint8_t* data, size_t length) {
    (void)data;
    (void)length;
    return false;
  }
};

// The ESP32's own measurements, merged into the MCX's telemetry. Null: not installed, or failed.
struct EspReadings {
  Nullable<float> tempC;       // t_c, SHT45
  Nullable<float> rhPct;       // rh_pct, SHT45
  Nullable<float> massG;       // mass_g, load cell
  Nullable<float> rateGpm;     // rate_gpm, load cell
  Nullable<float> fogIdxVirt;  // fog_idx_virt, virtual fog sensor (M5b)
  Nullable<float> rssiDbm;     // Wi-Fi signal, for the status message
};

// Reads and completes the MCX's messages on their way to the station: the ESP32's models (M5b virtual fog,
// M5c electrode health: lib/edge_ai/esp_models.h) and the HV controller (M6: lib/controller). Optional
// (NodeGateway::setHook).
class TelemetryHook {
 public:
  virtual ~TelemetryHook() {}
  virtual void telemetry(JsonObject payload, uint32_t nowMs) = 0;  // may fill the fields the MCX left null
  virtual void event(JsonObjectConst payload, uint32_t nowMs) {
    (void)payload;
    (void)nowMs;
  }
  // A station command on its way to the MCX.
  virtual void command(JsonObjectConst cmd, uint64_t unixMs, uint32_t nowMs) {
    (void)cmd;
    (void)unixMs;
    (void)nowMs;
  }
  // Asked after each telemetry: true to send the MCX a level request (SET_HINT, valid ttlS).
  virtual bool levelRequest(float* level, uint32_t* ttlS, uint32_t nowMs) {
    (void)level;
    (void)ttlS;
    (void)nowMs;
    return false;
  }
};

// Two hooks in turn: the ESP32's models first, then the controller, which reads what they filled in.
class HookChain : public TelemetryHook {
 public:
  HookChain(TelemetryHook& first, TelemetryHook& second) : first_(first), second_(second) {}
  void telemetry(JsonObject payload, uint32_t nowMs) override {
    first_.telemetry(payload, nowMs);
    second_.telemetry(payload, nowMs);
  }
  void event(JsonObjectConst payload, uint32_t nowMs) override {
    first_.event(payload, nowMs);
    second_.event(payload, nowMs);
  }
  void command(JsonObjectConst cmd, uint64_t unixMs, uint32_t nowMs) override {
    first_.command(cmd, unixMs, nowMs);
    second_.command(cmd, unixMs, nowMs);
  }
  bool levelRequest(float* level, uint32_t* ttlS, uint32_t nowMs) override {
    return first_.levelRequest(level, ttlS, nowMs) || second_.levelRequest(level, ttlS, nowMs);
  }

 private:
  TelemetryHook& first_;
  TelemetryHook& second_;
};

struct GatewayConfig {
  const char* nodeId;                 // the same as the MCX's NODE_ID
  const char* fwVersion;              // the ESP32 firmware, added to the MCX's in the info message
  const char* const* sensorNames;     // the ESP32's sensors, added to the MCX's in the info message
  int sensorCount;
  uint32_t mcxSilentMs = 5000;        // no line from the MCX for this long: FAULT COMM_LOST
  uint32_t ackTimeoutMs = 3000;       // a command passed to the MCX without CMD_ACK: rejected
  uint32_t timeEveryMs = 5000;        // time line to the MCX
  int offlineTelemetryEvery = 30;     // offline, keep one telemetry in this many (one per 30 s)
  int replayPerPoll = 8;              // buffered messages sent per poll() once the broker is back
  uint32_t replayDelayMs = 2000;      // after a reconnect, time for the station's services to subscribe again
  // FOG_ALERT to and from the neighbours (part 3)
  uint32_t alertCooldownMs = 10000;   // at most one alert sent per 10 s (fog flickering at the threshold)
  int alertCopies = 3;                // each alert goes out this many times: a broadcast has no acknowledgement
  uint32_t alertCopyGapMs = 100;      // time between the copies
  uint32_t alertMaxAgeMs = 60000;     // an older alert is ignored (when both clocks are synced)
  float alertPrearmEtaS = 60.0f;      // the fog is expected here this soon after a neighbour saw it
  uint32_t prearmCooldownMs = 30000;  // at most one PREARM from alerts per 30 s
  bool radioOnLink = false;           // Wokwi: radio packets also travel on the link as {"link":"radio"} lines
};

// The ESP32-S3 of a two-chip node (architecture v2), M7 part 2: it joins the MCX A156 (safety controller,
// UART) to the station (MQTT over Wi-Fi). The same job as tools/serial_bridge.py on a PC, plus:
//   - one message counter (seq) for the whole node: the ESP32 numbers every telemetry and event it sends;
//   - the ESP32's own measurements (load cell, SHT45, Wi-Fi signal) are merged into the MCX's messages;
//   - while the broker is down, events (and one telemetry in 30) are kept and sent later with
//     "buffered": true; info and status are kept and sent again when the broker comes back;
//   - TARE is done here (the load cell is on the ESP32); other commands go to the MCX, and if the MCX
//     does not answer within 3 s (or is silent), the ESP32 answers "rejected" so the station always gets
//     a CMD_ACK;
//   - the time (NTP) goes to the MCX every 5 s, once the ESP32 has it;
//   - FOG_ALERT (part 3): when the MCX detects fog, the neighbours hear it by radio; when a neighbour's alert
//     comes in, this node's MCX gets a PREARM (cmd_id "fa-<node>-<seq>"), as if the station had sent it;
//   - the HV controller's level requests (M6, through the hook) go to the MCX as SET_HINT, cmd_id "m6-<n>";
//     the MCX's answers to them are counted here, not published (one every 10 s would flood the station).
// Plain logic with no hardware access, so it is tested on the PC (test/test_gateway).
class NodeGateway {
 public:
  typedef bool (*TareFn)();  // zeroes the load cell; false if there is none

  NodeGateway(const GatewayConfig& config, GatewayPort& port);

  void setTare(TareFn tare) { tare_ = tare; }
  void setHook(TelemetryHook* hook) { hook_ = hook; }
  void setReadings(const EspReadings& readings) { readings_ = readings; }

  // One line received from the MCX. unixMs: the ESP32's clock.
  void fromMcx(const char* line, uint64_t unixMs, uint32_t nowMs);
  // One radio packet heard from a neighbour (ESP-NOW): a FOG_ALERT, or other traffic that is ignored.
  void fromRadio(const uint8_t* data, size_t length, uint64_t unixMs, bool timeSynced, uint32_t nowMs);
  // One MQTT message on a command topic (aeroion/<node>/cmd or aeroion/all/cmd).
  void fromBroker(const char* topic, const char* payload, size_t length, uint64_t unixMs, bool timeSynced,
                  uint32_t nowMs);
  // Call every loop: broker up/down, buffered messages, time for the MCX, silence, unanswered commands.
  // The nowMs given to fromMcx, fromBroker and poll may be a little out of order (read at different times).
  void poll(uint64_t unixMs, bool timeSynced, uint32_t nowMs);
  // An event of the ESP32 itself (e.g. a restart by the watchdog).
  void emit(const NodeEvent& event, uint64_t unixMs);

  const char* cmdTopic() const { return cmdTopic_; }
  const char* statusTopic() const { return statusTopic_; }
  void lastWill(char* out, size_t cap) const;  // status payload with online false, for the MQTT connect

  bool mcxAlive() const { return mcxAlive_; }
  int buffered() const { return buffer_.count(); }
  uint32_t dropped() const { return buffer_.dropped(); }
  uint32_t published() const { return published_; }
  uint32_t forwarded() const { return forwarded_; }
  uint32_t alertsSent() const { return alertsSent_; }
  uint32_t alertsHeard() const { return alertsHeard_; }
  uint32_t levelRequests() const { return levelRequests_; }  // M6's SET_HINTs sent to the MCX

 private:
  static const int kPending = 8;
  static const size_t kMcxLineMax = 4095;  // LINK_LINE_MAX on the MCX
  struct Pending {
    char cmdId[37];
    uint32_t sentMs;
    bool active;
  };

  void send(const char* kind, const char* payload, bool retain, bool keepWhenOffline, bool isEvent);
  void answer(const char* cmdId, aeroion_ack_t result, const char* reason, uint64_t unixMs);
  void tare(const char* cmdId, const char* version, bool tsOk, uint64_t ts, uint32_t ttlS, uint64_t unixMs,
            bool timeSynced);
  void addPending(const char* cmdId, uint32_t nowMs);
  void clearPending(const char* cmdId);
  void republishRetained();
  void topicOf(char* out, size_t cap, const char* kind) const;
  void startFogAlert(JsonObjectConst event, uint64_t unixMs, uint32_t nowMs);
  void sendAlertCopy(uint32_t nowMs);
  void radioOnLink(const uint8_t* packet);
  void requestLevel(float level, uint32_t ttlS, uint64_t unixMs);

  GatewayConfig config_;
  GatewayPort& port_;
  OfflineBuffer buffer_;
  TareFn tare_;
  TelemetryHook* hook_;
  EspReadings readings_;
  char cmdTopic_[48];
  char statusTopic_[48];
  uint32_t seq_;
  bool connected_;
  uint32_t connectedAtMs_;
  bool mcxAlive_;
  bool mcxEverHeard_;
  bool warnedNodeId_;
  uint32_t lastMcxMs_;
  uint32_t lastTimeLineMs_;
  bool timeLineSent_;
  int offlineTelemetry_;
  Pending pending_[kPending];
  char info_[OfflineBuffer::kMaxPayload];    // the latest info and status, sent again after a reconnect
  char status_[OfflineBuffer::kMaxPayload];
  char text_[kMcxLineMax + 1];               // the line being built (a command can be 4 kB)
  uint32_t published_;
  uint32_t forwarded_;
  bool timeSynced_;                     // the ESP32's clock, as of the last poll()
  FogAlertFilter alertFilter_;
  uint8_t alertPacket_[kFogAlertSize];  // the alert being sent
  uint16_t alertSeq_;
  int alertCopiesLeft_;
  uint32_t nextCopyMs_;
  bool alertSent_;
  uint32_t lastAlertMs_;
  bool radioWarned_;
  bool prearmSent_;
  uint32_t lastPrearmMs_;
  uint32_t alertsSent_;
  uint32_t alertsHeard_;
  uint32_t levelRequests_;
};