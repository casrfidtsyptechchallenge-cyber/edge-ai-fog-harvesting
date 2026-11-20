#pragma once
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "node_event.h"
#include "telemetry.h"

// Build message payloads with exactly the fields of the contract (contract/schemas/*.schema.json).
// The same payloads go to the serial port now (M1) and to MQTT later (M7).

void buildTelemetryPayload(JsonObject out, const NodeIdentity& id, uint64_t tsMs, uint32_t seq,
                           const NodeTelemetry& t);

void buildStatusPayload(JsonObject out, const NodeIdentity& id, uint64_t tsMs, uint32_t uptimeS,
                        bool timeSynced);

void buildInfoPayload(JsonObject out, const NodeIdentity& id, uint64_t tsMs, const char* const* sensorNames,
                      int sensorCount);


void buildEventPayload(JsonObject out, const NodeIdentity& id, uint64_t tsMs, uint32_t seq, const NodeEvent& e);

// One serial line: {"topic": "...", "retain": false, "payload": {...}}, the format read by
// tools/serial_capture.py and tools/contract_check.py. Returns the length written, or 0 if `cap` is too small.
size_t writeLine(char* out, size_t cap, const char* topic, bool retain, JsonObjectConst payload);