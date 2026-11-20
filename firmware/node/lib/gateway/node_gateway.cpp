#include "node_gateway.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "telemetry_json.h"

namespace {

double roundTo(float value, int decimals) {
  double scale = pow(10.0, decimals);
  return round(static_cast<double>(value) * scale) / scale;
}

// The ESP32's value replaces the MCX's null; with no value of its own, the MCX's stays.
void merge(JsonObject payload, const char* key, const Nullable<float>& value, int decimals) {
  if (value.has() && isfinite(value.get())) payload[key] = roundTo(value.get(), decimals);
}

// True when `span` ms have passed from `since` to `now`. Signed: a `since` read a moment after `now`
// (an earlier millis() in the same loop) counts as no time passed, not as 49 days.
bool elapsed(uint32_t now, uint32_t since, uint32_t span) {
  return static_cast<int32_t>(now - since) >= static_cast<int32_t>(span);
}

// "aeroion/n01/telemetry" -> node "n01", kind "telemetry". False for anything else.
bool splitTopic(const char* topic, char* node, size_t nodeCap, const char** kind) {
  static const char kRoot[] = "aeroion/";
  if (topic == nullptr || strncmp(topic, kRoot, sizeof kRoot - 1) != 0) return false;
  const char* start = topic + sizeof kRoot - 1;
  const char* slash = strchr(start, '/');
  if (slash == nullptr || static_cast<size_t>(slash - start) >= nodeCap) return false;
  memcpy(node, start, slash - start);
  node[slash - start] = '\0';
  *kind = slash + 1;
  return true;
}

}  // namespace

NodeGateway::NodeGateway(const GatewayConfig& config, GatewayPort& port)
    : config_(config),
      port_(port),
      tare_(nullptr),
      hook_(nullptr),
      seq_(0),
      connected_(false),
      connectedAtMs_(0),
      mcxAlive_(false),
      mcxEverHeard_(false),
      warnedNodeId_(false),
      lastMcxMs_(0),
      lastTimeLineMs_(0),
      timeLineSent_(false),
      offlineTelemetry_(0),
      published_(0),
      forwarded_(0),
      timeSynced_(false),
      alertSeq_(0),
      alertCopiesLeft_(0),
      nextCopyMs_(0),
      alertSent_(false),
      lastAlertMs_(0),
      radioWarned_(false),
      prearmSent_(false),
      lastPrearmMs_(0),
      alertsSent_(0),
      alertsHeard_(0),
      levelRequests_(0) {
  snprintf(cmdTopic_, sizeof cmdTopic_, AEROION_TOPIC_FMT, config.nodeId, AEROION_KIND_CMD);
  snprintf(statusTopic_, sizeof statusTopic_, AEROION_TOPIC_FMT, config.nodeId, AEROION_KIND_STATUS);
  memset(pending_, 0, sizeof pending_);
  memset(alertPacket_, 0, sizeof alertPacket_);
  info_[0] = '\0';
  status_[0] = '\0';
}

void NodeGateway::lastWill(char* out, size_t cap) const {
  snprintf(out, cap, "{\"schema_version\":\"" AEROION_SCHEMA_VERSION "\",\"node_id\":\"%s\",\"online\":false}",
           config_.nodeId);
}

void NodeGateway::topicOf(char* out, size_t cap, const char* kind) const {
  snprintf(out, cap, AEROION_TOPIC_FMT, config_.nodeId, kind);
}

// ---------------------------------------------------------------- MCX -> broker

void NodeGateway::fromMcx(const char* line, uint64_t unixMs, uint32_t nowMs) {
  lastMcxMs_ = nowMs;
  if (!mcxAlive_) {
    mcxAlive_ = true;
    port_.log(mcxEverHeard_ ? "# MCX answering again" : "# MCX (safety controller) found on the link");
    mcxEverHeard_ = true;
  }
  if (line[0] != '{') return;  // the MCX sends people's lines on USB only; anything else here is noise

  JsonDocument doc;
  if (deserializeJson(doc, line)) {
    port_.log("# from the MCX: a line that is not JSON, ignored");
    return;
  }
  const char* link = doc["link"];
  if (link != nullptr) {  // Wokwi only: a radio packet from a "neighbour" played by tools/mcx_sim.py
    const char* hex = doc["hex"];
    uint8_t packet[kFogAlertSize];
    size_t n = 0;
    while (config_.radioOnLink && strcmp(link, "radio") == 0 && hex != nullptr && n < sizeof packet &&
           hex[2 * n] != '\0' && hex[2 * n + 1] != '\0') {
      unsigned value = 0;
      if (sscanf(hex + 2 * n, "%2x", &value) != 1) break;
      packet[n++] = static_cast<uint8_t>(value);
    }
    if (n > 0) fromRadio(packet, n, unixMs, timeSynced_, nowMs);
    return;
  }
  char node[8];
  const char* kind = nullptr;
  JsonObject payload = doc["payload"];
  if (!splitTopic(doc["topic"], node, sizeof node, &kind) || payload.isNull()) {
    port_.log("# from the MCX: a JSON line that is not a contract message, ignored");
    return;
  }
  if (strcmp(node, config_.nodeId) != 0 && !warnedNodeId_) {
    warnedNodeId_ = true;
    char text[120];
    snprintf(text, sizeof text, "# WARNING: the MCX is %s and this ESP32 is %s: set the same NODE_ID in both",
             node, config_.nodeId);
    port_.log(text);
  }
  if (strcmp(kind, AEROION_KIND_TELEMETRY) == 0) {
    payload["seq"] = ++seq_;
    merge(payload, "t_c", readings_.tempC, 2);
    merge(payload, "rh_pct", readings_.rhPct, 1);
    merge(payload, "mass_g", readings_.massG, 2);
    merge(payload, "rate_gpm", readings_.rateGpm, 3);
    merge(payload, "fog_idx_virt", readings_.fogIdxVirt, 3);
    if (hook_ != nullptr) hook_->telemetry(payload, nowMs);  // the ESP32's models (M5b, M5c) and M6
    bool keep = offlineTelemetry_++ % config_.offlineTelemetryEvery == 0;  // only counts while offline
    if (connected_) offlineTelemetry_ = 0;
    serializeJson(payload, text_, sizeof text_);
    send(kind, text_, false, keep, false);
    float level;
    uint32_t ttlS;
    if (hook_ != nullptr && hook_->levelRequest(&level, &ttlS, nowMs)) requestLevel(level, ttlS, unixMs);
  } else if (strcmp(kind, AEROION_KIND_EVENT) == 0) {
    if (payload["type"] == "CMD_ACK" && strncmp(payload["data"]["cmd_id"] | "", "m6-", 3) == 0) {
      if (hook_ != nullptr) hook_->event(payload, nowMs);  // the answer to M6's own request: counted there
      if (payload["data"]["result"] != "accepted") {
        char text[120];
        snprintf(text, sizeof text, "# M6 level request refused by the MCX: %.64s",
                 static_cast<const char*>(payload["data"]["reason"] | ""));
        port_.log(text);
      }
      return;  // not published, and no seq used: the station would see a gap
    }
    payload["seq"] = ++seq_;
    if (payload["type"] == "CMD_ACK") clearPending(payload["data"]["cmd_id"]);
    if (hook_ != nullptr) hook_->event(payload, nowMs);
    serializeJson(payload, text_, sizeof text_);
    send(kind, text_, false, true, true);
    if (payload["type"] == "FOG_DETECTED") startFogAlert(payload, unixMs, nowMs);  // tell the neighbours
  } else if (strcmp(kind, AEROION_KIND_STATUS) == 0) {
    payload["buffered_msgs"] = buffer_.count();
    if (readings_.rssiDbm.has()) payload["rssi_dbm"] = roundTo(readings_.rssiDbm.get(), 0);
    serializeJson(payload, status_, sizeof status_);
    send(kind, status_, true, false, false);
  } else if (strcmp(kind, AEROION_KIND_INFO) == 0) {
    const char* mcxFw = payload["fw"] | "";
    char fw[33];
    if (strlen(mcxFw) + 1 + strlen(config_.fwVersion) < sizeof fw) {
      snprintf(fw, sizeof fw, "%s+%s", mcxFw, config_.fwVersion);  // e.g. 0.4.0-mcx+0.5.0-esp
      payload["fw"] = fw;
    }
    JsonArray sensors = payload["sensors"];
    for (int i = 0; i < config_.sensorCount && !sensors.isNull() && sensors.size() < 16; ++i) {
      sensors.add(config_.sensorNames[i]);
    }
    serializeJson(payload, info_, sizeof info_);
    send(kind, info_, true, false, false);
  } else if (strcmp(kind, "wave") == 0) {
    payload["seq"] = ++seq_;
    serializeJson(payload, text_, sizeof text_);
    send(kind, text_, false, false, false);  // too big to keep while offline
  } else {
    port_.log("# from the MCX: unknown message kind, ignored");
  }
}

// Publishes now, or keeps the message for later with "buffered": true (contract rule 5). Retained messages
// (info, status) are not queued: their latest copy is sent again when the broker comes back.
void NodeGateway::send(const char* kind, const char* payload, bool retain, bool keepWhenOffline, bool isEvent) {
  char topic[48];
  topicOf(topic, sizeof topic, kind);
  if (connected_ && port_.publish(topic, payload, retain)) {
    published_++;
    return;
  }
  if (!keepWhenOffline) return;
  char kept[OfflineBuffer::kMaxPayload];
  int n = snprintf(kept, sizeof kept, "{\"buffered\":true%s%s", payload[1] == '}' ? "" : ",", payload + 1);
  if (n >= static_cast<int>(sizeof kept) || !buffer_.push(kept, isEvent)) {
    port_.log("# message too long to keep offline: lost");
  }
}

// ---------------------------------------------------------------- broker -> MCX

void NodeGateway::fromBroker(const char* topic, const char* payload, size_t length, uint64_t unixMs,
                             bool timeSynced, uint32_t nowMs) {
  char topicCopy[48];  // the MQTT client may reuse its buffer for the next message
  NodeEvent::copyText(topicCopy, sizeof topicCopy, topic);
  JsonDocument cmd;
  if (deserializeJson(cmd, payload, length) || !cmd.is<JsonObject>()) {
    port_.log("# a command that is not a JSON object: ignored");
    return;
  }
  const char* cmdId = cmd["cmd_id"];
  bool canAnswer = cmdId != nullptr && cmdId[0] != '\0' && strlen(cmdId) <= 36;
  const char* type = cmd["type"] | "?";
  char text[128];
  snprintf(text, sizeof text, "# command %.16s %.36s", type, canAnswer ? cmdId : "(no cmd_id)");
  port_.log(text);
  if (hook_ != nullptr) hook_->command(cmd.as<JsonObjectConst>(), unixMs, nowMs);  // M6 sees the station's hints

  if (strcmp(type, "TARE") == 0) {  // the load cell is here
    if (canAnswer) {
      tare(cmdId, cmd["schema_version"], cmd["ts"].is<uint64_t>(), cmd["ts"] | 0ULL, cmd["ttl_s"] | 0U, unixMs,
           timeSynced);
    }
    return;
  }
  if (!mcxAlive_) {
    if (canAnswer) answer(cmdId, AEROION_ACK_REJECTED, "safety controller (MCX) not answering", unixMs);
    return;
  }
  JsonDocument line;
  line["topic"] = topicCopy;
  line["retain"] = false;
  line["payload"] = cmd;
  if (measureJson(line) > kMcxLineMax) {
    if (canAnswer) answer(cmdId, AEROION_ACK_REJECTED, "command too long for the node (4095 characters)", unixMs);
    return;
  }
  serializeJson(line, text_, sizeof text_);
  port_.toMcx(text_);
  forwarded_++;
  if (canAnswer) addPending(cmdId, nowMs);
}

void NodeGateway::tare(const char* cmdId, const char* version, bool tsOk, uint64_t ts, uint32_t ttlS,
                       uint64_t unixMs, bool timeSynced) {
  char text[80];
  if (version == nullptr || strncmp(version, "1.", 2) != 0) {
    answer(cmdId, AEROION_ACK_REJECTED, "schema_version not supported", unixMs);
  } else if (!tsOk || ttlS < 1 || ttlS > 600) {
    answer(cmdId, AEROION_ACK_REJECTED, "ts and ttl_s (1 to 600) needed", unixMs);
  } else if (timeSynced && unixMs > ts && unixMs - ts >= ttlS * 1000ULL) {
    snprintf(text, sizeof text, "sent %lu s ago, valid for %lu s", static_cast<unsigned long>((unixMs - ts) / 1000),
             static_cast<unsigned long>(ttlS));
    answer(cmdId, AEROION_ACK_EXPIRED, text, unixMs);
  } else if (tare_ == nullptr || !tare_()) {
    answer(cmdId, AEROION_ACK_REJECTED, "no working load cell on this node", unixMs);
  } else {
    answer(cmdId, AEROION_ACK_ACCEPTED, "", unixMs);  // mass_g starts again from 0 within a second
  }
}

void NodeGateway::answer(const char* cmdId, aeroion_ack_t result, const char* reason, uint64_t unixMs) {
  emit(NodeEvent::cmdAck(cmdId, result, result == AEROION_ACK_ACCEPTED ? "" : reason), unixMs);
}

void NodeGateway::emit(const NodeEvent& event, uint64_t unixMs) {
  NodeIdentity id = {config_.nodeId, config_.fwVersion, 0.0f, 0.0f};
  JsonDocument payload;
  buildEventPayload(payload.to<JsonObject>(), id, unixMs, ++seq_, event);
  serializeJson(payload, text_, sizeof text_);
  send(AEROION_KIND_EVENT, text_, false, true, true);
}

void NodeGateway::addPending(const char* cmdId, uint32_t nowMs) {
  Pending* slot = &pending_[0];
  for (int i = 0; i < kPending; ++i) {
    if (!pending_[i].active) {
      slot = &pending_[i];
      break;
    }
    if (static_cast<int32_t>(pending_[i].sentMs - slot->sentMs) < 0) slot = &pending_[i];  // all busy: oldest
  }
  NodeEvent::copyText(slot->cmdId, sizeof slot->cmdId, cmdId);
  slot->sentMs = nowMs;
  slot->active = true;
}

void NodeGateway::clearPending(const char* cmdId) {
  if (cmdId == nullptr) return;
  for (int i = 0; i < kPending; ++i) {
    if (pending_[i].active && strcmp(pending_[i].cmdId, cmdId) == 0) pending_[i].active = false;
  }
}

// ---------------------------------------------------------------- FOG_ALERT, node to node

void NodeGateway::startFogAlert(JsonObjectConst event, uint64_t unixMs, uint32_t nowMs) {
  if (alertSent_ && !elapsed(nowMs, lastAlertMs_, config_.alertCooldownMs)) return;  // fog flickering
  FogAlert alert;
  alert.nodeNum = nodeNumberOf(config_.nodeId);
  alert.tsMs = unixMs;  // this chip's clock, which timeSynced describes (the MCX gets the time a few s later)
  alert.seq = ++alertSeq_;
  alert.fogIdx = event["data"]["fog_idx"] | 0.0f;
  alert.timeSynced = timeSynced_;
  alert.virtualSource = event["data"]["source"] == "virtual";
  if (!packFogAlert(alert, alertPacket_)) return;
  alertSent_ = true;
  lastAlertMs_ = nowMs;
  alertsSent_++;
  alertCopiesLeft_ = config_.alertCopies;
  sendAlertCopy(nowMs);  // the first copy at once, the others from poll()
  char text[96];
  snprintf(text, sizeof text, "# FOG_ALERT to the neighbours: seq %u, fog %d %%", static_cast<unsigned>(alert.seq),
           static_cast<int>(alert.fogIdx * 100.0f + 0.5f));
  port_.log(text);
  if (config_.radioOnLink) radioOnLink(alertPacket_);
}

void NodeGateway::sendAlertCopy(uint32_t nowMs) {
  if (alertCopiesLeft_ <= 0) return;
  alertCopiesLeft_--;
  nextCopyMs_ = nowMs + config_.alertCopyGapMs;
  if (!port_.radioSend(alertPacket_, kFogAlertSize) && !radioWarned_) {
    radioWarned_ = true;
    port_.log("# radio (ESP-NOW) not available: FOG_ALERT not sent by radio");
  }
}

// Wokwi only: the packet also goes on the link, where tools/mcx_sim.py shows it.
void NodeGateway::radioOnLink(const uint8_t* packet) {
  char hex[2 * kFogAlertSize + 1];
  for (size_t i = 0; i < kFogAlertSize; ++i) snprintf(hex + 2 * i, 3, "%02X", packet[i]);
  snprintf(text_, sizeof text_, "{\"link\":\"radio\",\"hex\":\"%s\"}", hex);
  port_.toMcx(text_);
}

void NodeGateway::fromRadio(const uint8_t* data, size_t length, uint64_t unixMs, bool timeSynced, uint32_t nowMs) {
  FogAlert alert;
  FogAlertRead read = readFogAlert(data, length, &alert);
  if (read == kAlertWrongSize || read == kAlertForeign) return;  // other ESP-NOW traffic
  if (read != kAlertOk) {
    port_.log("# radio: a FOG_ALERT of another version, ignored");
    return;
  }
  if (alert.nodeNum == nodeNumberOf(config_.nodeId) || !alertFilter_.isNew(alert)) return;  // ours, or a copy
  alertsHeard_++;
  char source[8];
  char text[128];
  snprintf(source, sizeof source, "n%02d", alert.nodeNum);
  bool bothSynced = timeSynced && alert.timeSynced;
  if (bothSynced) {
    int64_t ageMs = static_cast<int64_t>(unixMs - alert.tsMs);
    if (ageMs > static_cast<int64_t>(config_.alertMaxAgeMs) || -ageMs > static_cast<int64_t>(config_.alertMaxAgeMs)) {
      snprintf(text, sizeof text, "# FOG_ALERT from %s, %ld s old: ignored", source, static_cast<long>(ageMs / 1000));
      port_.log(text);
      return;
    }
  }
  if (!mcxAlive_) {
    snprintf(text, sizeof text, "# FOG_ALERT from %s: the MCX is silent, it cannot pre-arm", source);
    port_.log(text);
    return;
  }
  if (prearmSent_ && !elapsed(nowMs, lastPrearmMs_, config_.prearmCooldownMs)) {
    snprintf(text, sizeof text, "# FOG_ALERT from %s: already pre-armed by a recent alert", source);
    port_.log(text);
    return;
  }
  prearmSent_ = true;
  lastPrearmMs_ = nowMs;

  // A PREARM for this node's MCX, as the station sends it; the MCX checks it and answers with a CMD_ACK.
  char cmdId[24];
  snprintf(cmdId, sizeof cmdId, "fa-%s-%u", source, static_cast<unsigned>(alert.seq));
  JsonDocument line;
  line["topic"] = static_cast<const char*>(cmdTopic_);
  line["retain"] = false;
  JsonObject cmd = line["payload"].to<JsonObject>();
  cmd["schema_version"] = AEROION_SCHEMA_VERSION;
  cmd["cmd_id"] = static_cast<const char*>(cmdId);
  cmd["ts"] = bothSynced ? alert.tsMs : unixMs;  // eta_s counts from the neighbour's detection
  cmd["ttl_s"] = config_.alertMaxAgeMs / 1000;  // the same limit as above: the MCX refuses an older alert too
  cmd["type"] = "PREARM";
  JsonObject params = cmd["params"].to<JsonObject>();
  params["eta_s"] = config_.alertPrearmEtaS;
  params["source_node"] = static_cast<const char*>(source);
  serializeJson(line, text_, sizeof text_);
  port_.toMcx(text_);
  snprintf(text, sizeof text, "# FOG_ALERT from %s (fog %d %%): PREARM sent to the MCX", source,
           static_cast<int>(alert.fogIdx * 100.0f + 0.5f));
  port_.log(text);
}

// ---------------------------------------------------------------- HV controller (M6) -> MCX

// The controller's level, as the station's SET_HINT: the MCX checks it like any command, the safety core
// still limits it, and it expires after ttlS (then the MCX's default level applies).
void NodeGateway::requestLevel(float level, uint32_t ttlS, uint64_t unixMs) {
  if (!mcxAlive_) return;
  char cmdId[16];
  snprintf(cmdId, sizeof cmdId, "m6-%lu", static_cast<unsigned long>(++levelRequests_));
  JsonDocument line;
  line["topic"] = static_cast<const char*>(cmdTopic_);
  line["retain"] = false;
  JsonObject cmd = line["payload"].to<JsonObject>();
  cmd["schema_version"] = AEROION_SCHEMA_VERSION;
  cmd["cmd_id"] = static_cast<const char*>(cmdId);
  cmd["ts"] = unixMs;
  cmd["ttl_s"] = ttlS;
  cmd["type"] = "SET_HINT";
  cmd["params"]["hv_level"] = roundTo(level, 3);
  serializeJson(line, text_, sizeof text_);
  port_.toMcx(text_);
}

// ---------------------------------------------------------------- every loop

void NodeGateway::republishRetained() {
  if (info_[0] != '\0') send(AEROION_KIND_INFO, info_, true, false, false);
  if (status_[0] != '\0') {
    JsonDocument status;
    if (!deserializeJson(status, status_)) {
      status["buffered_msgs"] = buffer_.count();
      serializeJson(status, status_, sizeof status_);
    }
    send(AEROION_KIND_STATUS, status_, true, false, false);
  }
}

void NodeGateway::poll(uint64_t unixMs, bool timeSynced, uint32_t nowMs) {
  timeSynced_ = timeSynced;
  if (alertCopiesLeft_ > 0 && elapsed(nowMs, nextCopyMs_, 0)) sendAlertCopy(nowMs);
  bool connected = port_.mqttConnected();
  if (connected && !connected_) {
    connected_ = true;
    connectedAtMs_ = nowMs;
    offlineTelemetry_ = 0;
    char text[80];
    snprintf(text, sizeof text, "# MQTT connected, %d buffered message(s) to send", buffer_.count());
    port_.log(text);
    republishRetained();
  } else if (!connected && connected_) {
    connected_ = false;
    port_.log("# MQTT connection lost: keeping events until it is back");
    emit(NodeEvent::faultEvent(AEROION_FAULT_COMM_LOST, "MQTT broker lost; events kept and sent later"), unixMs);
  }

  // Buffered messages, a few per loop so the link to the MCX keeps being read, from 2 s after the reconnect.
  char topic[48];
  bool replay = connected_ && elapsed(nowMs, connectedAtMs_, config_.replayDelayMs);
  for (int i = 0; i < config_.replayPerPoll && replay && buffer_.count() > 0; ++i) {
    topicOf(topic, sizeof topic, buffer_.frontIsEvent() ? AEROION_KIND_EVENT : AEROION_KIND_TELEMETRY);
    if (!port_.publish(topic, buffer_.frontPayload(), false)) break;
    buffer_.pop();
    published_++;
  }

  // The time for the MCX, only once the ESP32 itself has it: a wrong time is worse than none.
  if (timeSynced && (!timeLineSent_ || elapsed(nowMs, lastTimeLineMs_, config_.timeEveryMs))) {
    timeLineSent_ = true;
    lastTimeLineMs_ = nowMs;
    JsonDocument line;
    line["link"] = "time";
    line["ts"] = unixMs;
    serializeJson(line, text_, sizeof text_);
    port_.toMcx(text_);
  }

  if (mcxAlive_ && elapsed(nowMs, lastMcxMs_, config_.mcxSilentMs)) {
    mcxAlive_ = false;
    char text[96];
    snprintf(text, sizeof text, "safety controller (MCX) silent for %lu s: check the UART wires",
             static_cast<unsigned long>(config_.mcxSilentMs / 1000));
    port_.log("# MCX silent: commands are refused until it answers again");
    emit(NodeEvent::faultEvent(AEROION_FAULT_COMM_LOST, text), unixMs);
  }

  for (int i = 0; i < kPending; ++i) {
    if (pending_[i].active && elapsed(nowMs, pending_[i].sentMs, config_.ackTimeoutMs)) {
      pending_[i].active = false;
      answer(pending_[i].cmdId, AEROION_ACK_REJECTED, "no answer from the safety controller (MCX)", unixMs);
    }
  }
}