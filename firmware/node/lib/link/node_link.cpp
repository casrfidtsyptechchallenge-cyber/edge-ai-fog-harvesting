#include "node_link.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

NodeLink::NodeLink(const char* nodeId, UnixClock& clock, CommandHandler& commands, EventSink& events,
                   uint32_t silentMs)
    : clock_(clock),
      commands_(commands),
      events_(events),
      silentMs_(silentMs),
      up_(false),
      cameUp_(false),
      lastTimeMs_(0),
      lastError_("") {
  snprintf(cmdTopic_, sizeof cmdTopic_, AEROION_TOPIC_FMT, nodeId, AEROION_KIND_CMD);
  memset(lines_, 0, sizeof lines_);
}

NodeLink::LineKind NodeLink::counted(LineKind kind) {
  lines_[kind]++;
  return kind;
}

NodeLink::LineKind NodeLink::handleLine(const char* line, uint32_t nowMs) {
  JsonDocument doc;  // freed on return; a 64-badge SYNC_ALLOWLIST needs a few kB of heap here
  DeserializationError error = deserializeJson(doc, line);
  if (error) {
    lastError_ = error.c_str();
    return counted(kBadJson);
  }
  if (!doc.is<JsonObjectConst>()) {
    lastError_ = "not a JSON object";
    return counted(kBadJson);
  }
  JsonObjectConst message = doc.as<JsonObjectConst>();

  const char* link = message["link"];
  if (link != nullptr) {
    JsonVariantConst ts = message["ts"];
    if (strcmp(link, "time") != 0 || !ts.is<uint64_t>() || !clock_.set(ts.as<uint64_t>(), nowMs)) {
      return counted(kUnknown);
    }
    lastTimeMs_ = nowMs;
    if (!up_) {
      up_ = true;
      cameUp_ = true;
    }
    return counted(kTime);
  }

  const char* topic = message["topic"];
  JsonVariantConst payload = message["payload"];
  if (topic == nullptr || !payload.is<JsonObjectConst>()) return counted(kUnknown);
  if (strcmp(topic, cmdTopic_) != 0 && strcmp(topic, AEROION_TOPIC_BROADCAST_CMD) != 0) return counted(kNotForUs);
  commands_.handle(payload.as<JsonObjectConst>(), clock_.now(nowMs), clock_.synced(nowMs), nowMs);
  return counted(kCommand);
}

void NodeLink::poll(uint32_t nowMs) {
  if (!up_ || nowMs - lastTimeMs_ < silentMs_) return;
  up_ = false;
  char text[96];
  snprintf(text, sizeof text, "no time line from the station link for %lu s: commands cannot arrive",
           static_cast<unsigned long>(silentMs_ / 1000));
  events_.emit(NodeEvent::faultEvent(AEROION_FAULT_COMM_LOST, text));
}

bool NodeLink::takeCameUp() {
  bool cameUp = cameUp_;
  cameUp_ = false;
  return cameUp;
}

void NodeLink::printStatus(PrintFn print, uint32_t nowMs) {
  char text[160];
  if (up_) {
    snprintf(text, sizeof text, "# link up: last time line %lu ms ago\r\n",
             static_cast<unsigned long>(nowMs - lastTimeMs_));
  } else {
    snprintf(text, sizeof text, "# link down: %s\r\n",
             lines_[kTime] ? "no time line for a while" : "no time line since boot (start the ESP32 or the bridge)");
  }
  print(text);
  // Seconds, not milliseconds: the small C library of the MCX cannot print 64-bit numbers.
  snprintf(text, sizeof text, "# clock %s, Unix time %lu s\r\n",
           clock_.synced(nowMs) ? "synced" : (clock_.everSet() ? "NOT synced for 10 min" : "NOT synced (build time)"),
           static_cast<unsigned long>(clock_.now(nowMs) / 1000ULL));
  print(text);
  snprintf(text, sizeof text, "# lines in: %lu time, %lu command, %lu for other nodes, %lu unknown, %lu bad JSON%s%s\r\n",
           static_cast<unsigned long>(lines_[kTime]), static_cast<unsigned long>(lines_[kCommand]),
           static_cast<unsigned long>(lines_[kNotForUs]), static_cast<unsigned long>(lines_[kUnknown]),
           static_cast<unsigned long>(lines_[kBadJson]), lines_[kBadJson] ? ", last: " : "",
           lines_[kBadJson] ? lastError_ : "");
  print(text);
  snprintf(text, sizeof text, "# answers: %lu accepted, %lu rejected, %lu expired\r\n",
           static_cast<unsigned long>(commands_.count(AEROION_ACK_ACCEPTED)),
           static_cast<unsigned long>(commands_.count(AEROION_ACK_REJECTED)),
           static_cast<unsigned long>(commands_.count(AEROION_ACK_EXPIRED)));
  print(text);
  if (commands_.hintActive(nowMs)) {
    snprintf(text, sizeof text, "# SET_HINT from the station: HV level %d %%\r\n",
             static_cast<int>(commands_.requestedLevel(0.0f, nowMs) * 100.0f + 0.5f));
    print(text);
  }
}