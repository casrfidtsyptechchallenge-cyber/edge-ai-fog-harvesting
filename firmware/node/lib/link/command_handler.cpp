#include "command_handler.h"

#include <stdio.h>
#include <string.h>

#include "allowlist_sync.h"

namespace {

const uint32_t kMaxTtlS = 600;  // cmd.schema.json: ttl_s is 1 to 600

// The node runs commands of its own major contract version (any "1.x" works with "1.2").
bool sameMajorVersion(const char* version) {
  const char* dot = strchr(AEROION_SCHEMA_VERSION, '.');
  size_t n = static_cast<size_t>(dot - AEROION_SCHEMA_VERSION);
  return version != nullptr && strncmp(version, AEROION_SCHEMA_VERSION, n) == 0 && version[n] == '.';
}

SafetyReply refuse(const char* reason) {
  SafetyReply r;
  r.accepted = false;
  NodeEvent::copyText(r.reason, sizeof r.reason, reason);
  return r;
}

SafetyReply accept() {
  SafetyReply r;
  r.accepted = true;
  r.reason[0] = '\0';
  return r;
}

}  // namespace

bool commandTypeFromName(const char* name, aeroion_cmd_t* type) {
  if (name == nullptr) return false;
  for (int i = 0; i < AEROION_CMD__COUNT; ++i) {
    if (strcmp(name, AEROION_CMD_NAMES[i]) == 0) {
      *type = static_cast<aeroion_cmd_t>(i);
      return true;
    }
  }
  return false;
}

CommandHandler::CommandHandler(SafetyCore& safety, BadgeList& badges, EventSink& events)
    : safety_(safety),
      badges_(badges),
      events_(events),
      hint_(false),
      hintLevel_(0.0f),
      hintStartMs_(0),
      hintValidMs_(0),
      ackSent_(false),
      lastResult_(AEROION_ACK_ACCEPTED) {
  lastType_[0] = '\0';
  lastReason_[0] = '\0';
  memset(counts_, 0, sizeof counts_);
}

aeroion_ack_t CommandHandler::handle(JsonObjectConst cmd, uint64_t unixMs, bool timeSynced, uint32_t nowMs) {
  char text[96];  // cut to 64 characters in the CMD_ACK
  const char* cmdId = cmd["cmd_id"];
  const char* typeName = cmd["type"];
  NodeEvent::copyText(lastType_, sizeof lastType_, typeName ? typeName : "?");
  ackSent_ = false;
  if (cmdId == nullptr || cmdId[0] == '\0' || strlen(cmdId) > 36) {
    lastResult_ = AEROION_ACK_REJECTED;
    NodeEvent::copyText(lastReason_, sizeof lastReason_, "no usable cmd_id, so no answer");
    return lastResult_;
  }

  const char* version = cmd["schema_version"];
  if (!sameMajorVersion(version)) {
    snprintf(text, sizeof text, "schema_version %.12s not supported, node has " AEROION_SCHEMA_VERSION,
             version ? version : "missing");
    return answer(cmdId, AEROION_ACK_REJECTED, text);
  }
  aeroion_cmd_t type;
  if (!commandTypeFromName(typeName, &type)) return answer(cmdId, AEROION_ACK_REJECTED, "unknown command type");
  JsonVariantConst ts = cmd["ts"];
  JsonVariantConst ttl = cmd["ttl_s"];
  if (!ts.is<uint64_t>()) return answer(cmdId, AEROION_ACK_REJECTED, "ts must be Unix time in ms");
  uint32_t ttlS = ttl.is<uint32_t>() ? ttl.as<uint32_t>() : 0;
  if (ttlS < 1 || ttlS > kMaxTtlS) return answer(cmdId, AEROION_ACK_REJECTED, "ttl_s must be 1 to 600");
  JsonObjectConst params = cmd["params"];
  if (params.isNull()) return answer(cmdId, AEROION_ACK_REJECTED, "params missing");

  // Age of the command, once the clock can tell (a command "from the future" counts as new).
  uint32_t validMs = ttlS * 1000UL;
  uint32_t ageMs = 0;
  uint64_t sentMs = ts.as<uint64_t>();
  if (timeSynced && unixMs > sentMs) {
    uint64_t age = unixMs - sentMs;
    if (age >= validMs) {
      snprintf(text, sizeof text, "sent %lu s ago, valid for %lu s", static_cast<unsigned long>(age / 1000),
               static_cast<unsigned long>(ttlS));
      return answer(cmdId, AEROION_ACK_EXPIRED, text);
    }
    ageMs = static_cast<uint32_t>(age);
  }
  SafetyReply reply = run(type, params, ageMs, validMs, nowMs);
  return answer(cmdId, reply.accepted ? AEROION_ACK_ACCEPTED : AEROION_ACK_REJECTED, reply.reason);
}

SafetyReply CommandHandler::run(aeroion_cmd_t type, JsonObjectConst params, uint32_t ageMs, uint32_t validMs,
                                uint32_t nowMs) {
  switch (type) {
    case AEROION_CMD_LOCK: {
      const char* reason = params["reason"];
      if (reason == nullptr) return refuse("reason missing");
      return safety_.lock(reason);
    }
    case AEROION_CMD_PREARM: {
      JsonVariantConst eta = params["eta_s"];
      if (!eta.is<float>() || !params["source_node"].is<const char*>()) return refuse("eta_s and source_node needed");
      float etaS = eta.as<float>();
      if (!(etaS >= 0.0f && etaS <= 3600.0f)) return refuse("eta_s must be 0 to 3600");
      etaS -= static_cast<float>(ageMs) / 1000.0f;  // eta_s counts from ts, not from now
      return safety_.prearm(etaS > 0.0f ? etaS : 0.0f, nowMs);
    }
    case AEROION_CMD_SET_HINT: {
      JsonVariantConst level = params["hv_level"];
      if (!level.is<float>()) return refuse("hv_level missing");
      float value = level.as<float>();
      if (!(value >= 0.0f && value <= 1.0f)) return refuse("hv_level must be 0 to 1");
      hint_ = true;  // the safety core still limits it, as any request
      hintLevel_ = value;
      hintStartMs_ = nowMs;
      hintValidMs_ = validMs - ageMs;
      return accept();
    }
    case AEROION_CMD_SYNC_ALLOWLIST: {
      SafetyReply r = accept();
      r.accepted = applySyncAllowlist(params, badges_, r.reason, sizeof r.reason);
      return r;
    }
    case AEROION_CMD_TARE:
      return refuse("no load cell on the MCX: the node's ESP32 answers TARE");
    default:
      return refuse("unknown command type");
  }
}

aeroion_ack_t CommandHandler::answer(const char* cmdId, aeroion_ack_t result, const char* reason) {
  const char* text = result == AEROION_ACK_ACCEPTED ? "" : reason;  // the contract: empty when accepted
  lastResult_ = result;
  NodeEvent::copyText(lastReason_, sizeof lastReason_, text);
  events_.emit(NodeEvent::cmdAck(cmdId, result, text));
  ackSent_ = true;
  counts_[result]++;
  return result;
}

bool CommandHandler::hintActive(uint32_t nowMs) const { return hint_ && nowMs - hintStartMs_ < hintValidMs_; }

float CommandHandler::requestedLevel(float fallback, uint32_t nowMs) const {
  return hintActive(nowMs) ? hintLevel_ : fallback;
}

uint32_t CommandHandler::count(aeroion_ack_t result) const {
  return (static_cast<int>(result) >= 0 && result < AEROION_ACK__COUNT) ? counts_[result] : 0;
}