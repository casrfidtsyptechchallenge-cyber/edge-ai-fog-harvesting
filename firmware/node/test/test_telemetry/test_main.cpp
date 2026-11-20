// Tests for the message builders (lib/telemetry). They run on the PC: pio test -e native
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include <unity.h>

#include "telemetry_json.h"

static const NodeIdentity kId = {"n01", "test", 15.0f, 12.0f};
static const uint64_t kTs = 1791747040265ULL;

void setUp() {}
void tearDown() {}

static JsonDocument telemetryFor(const NodeTelemetry& t) {
  JsonDocument doc;
  buildTelemetryPayload(doc.to<JsonObject>(), kId, kTs, 7, t);
  return doc;
}

void test_telemetry_has_every_contract_field() {
  JsonDocument doc = telemetryFor(NodeTelemetry());
  const char* fields[] = {"schema_version", "node_id", "ts", "seq", "state", "hv_on", "hv_level", "fog_idx_opt",
                          "fog_idx_virt", "t_c", "rh_pct", "mass_g", "rate_gpm", "i_ua", "p_w", "e_wh",
                          "arc_risk", "health", "radar_presence", "lid_closed", "batt_pct"};
  for (const char* field : fields) TEST_ASSERT_TRUE_MESSAGE(doc[field].is<JsonVariantConst>(), field);
  TEST_ASSERT_EQUAL(21, static_cast<int>(doc.as<JsonObjectConst>().size()));
  TEST_ASSERT_EQUAL(3, static_cast<int>(doc["health"].as<JsonObjectConst>().size()));
}

void test_defaults_before_other_modules_exist() {
  JsonDocument doc = telemetryFor(NodeTelemetry());
  TEST_ASSERT_EQUAL_STRING(AEROION_SCHEMA_VERSION, doc["schema_version"]);
  TEST_ASSERT_EQUAL_STRING("LOCKED", doc["state"]);
  TEST_ASSERT_FALSE(doc["hv_on"].as<bool>());
  TEST_ASSERT_TRUE(doc["arc_risk"].isNull());
  TEST_ASSERT_TRUE(doc["health"]["score"].isNull());
}

void test_missing_sensors_are_null_not_zero() {
  JsonDocument doc = telemetryFor(NodeTelemetry());
  TEST_ASSERT_TRUE(doc["t_c"].isNull());
  TEST_ASSERT_TRUE(doc["mass_g"].isNull());
  TEST_ASSERT_TRUE(doc["e_wh"].isNull());
  TEST_ASSERT_TRUE(doc["radar_presence"].isNull());
  TEST_ASSERT_TRUE(doc["lid_closed"].isNull());
}

void test_sensor_values_are_copied_and_rounded() {
  NodeTelemetry t;
  t.sensors.tempC = 21.4567f;
  t.sensors.rhPct = 98.26f;
  t.sensors.massG = 4.8312f;
  t.sensors.lidClosed = true;
  t.sensors.radarPresence = false;
  JsonDocument doc = telemetryFor(t);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 21.46f, doc["t_c"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 98.3f, doc["rh_pct"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 4.83f, doc["mass_g"].as<float>());
  char text[600];
  serializeJson(doc, text, sizeof text);
  TEST_ASSERT_NOT_NULL(strstr(text, "\"t_c\":21.46,"));  // rounded in the JSON text itself
  TEST_ASSERT_TRUE(doc["lid_closed"].as<bool>());
  TEST_ASSERT_FALSE(doc["radar_presence"].isNull());
  TEST_ASSERT_FALSE(doc["radar_presence"].as<bool>());
}

void test_nan_becomes_null() {
  NodeTelemetry t;
  t.sensors.powerW = NAN;
  t.sensors.currentUa = INFINITY;
  JsonDocument doc = telemetryFor(t);
  TEST_ASSERT_TRUE(doc["p_w"].isNull());
  TEST_ASSERT_TRUE(doc["i_ua"].isNull());
}

void test_timestamp_keeps_all_64_bits() {
  JsonDocument doc = telemetryFor(NodeTelemetry());
  TEST_ASSERT_TRUE(doc["ts"].as<uint64_t>() == kTs);
  TEST_ASSERT_EQUAL(7, doc["seq"].as<int>());
}

void test_status_payload() {
  JsonDocument doc;
  buildStatusPayload(doc.to<JsonObject>(), kId, kTs, 42, false);
  TEST_ASSERT_TRUE(doc["online"].as<bool>());
  TEST_ASSERT_EQUAL(42, doc["uptime_s"].as<int>());
  TEST_ASSERT_FALSE(doc["time_synced"].as<bool>());
}

void test_info_payload_lists_sensors_and_position() {
  const char* names[] = {"SHT45", "HX711"};
  JsonDocument doc;
  buildInfoPayload(doc.to<JsonObject>(), kId, kTs, names, 2);
  TEST_ASSERT_EQUAL(2, static_cast<int>(doc["sensors"].size()));
  TEST_ASSERT_EQUAL_STRING("HX711", doc["sensors"][1]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 15.0f, doc["position"]["x_m"].as<float>());
  TEST_ASSERT_TRUE(doc["models"]["arc"].isNull());
}

void test_write_line_wraps_topic_and_payload() {
  JsonDocument payload;
  buildStatusPayload(payload.to<JsonObject>(), kId, kTs, 1, false);
  char line[512];
  size_t n = writeLine(line, sizeof line, "aeroion/n01/status", true, payload.as<JsonObjectConst>());
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL(n, strlen(line));
  JsonDocument back;
  TEST_ASSERT_TRUE(deserializeJson(back, line) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("aeroion/n01/status", back["topic"]);
  TEST_ASSERT_TRUE(back["retain"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("n01", back["payload"]["node_id"]);
}

void test_write_line_refuses_a_too_small_buffer() {
  JsonDocument payload;
  buildStatusPayload(payload.to<JsonObject>(), kId, kTs, 1, false);
  char line[20];
  TEST_ASSERT_EQUAL(0, static_cast<int>(writeLine(line, sizeof line, "aeroion/n01/status", true,
                                                  payload.as<JsonObjectConst>())));
}

// ---------------------------------------------------------------- events (M3)

static JsonDocument eventFor(const NodeEvent& e) {
  JsonDocument doc;
  buildEventPayload(doc.to<JsonObject>(), kId, kTs, 9, e);
  return doc;
}

void test_state_change_event() {
  JsonDocument doc = eventFor(NodeEvent::stateChange(AEROION_STATE_ARMED, AEROION_STATE_HARVEST, "fog detected"));
  TEST_ASSERT_EQUAL_STRING("STATE_CHANGE", doc["type"]);
  TEST_ASSERT_EQUAL(9, doc["seq"].as<int>());
  TEST_ASSERT_EQUAL_STRING("ARMED", doc["data"]["from"]);
  TEST_ASSERT_EQUAL_STRING("HARVEST", doc["data"]["to"]);
  TEST_ASSERT_EQUAL_STRING("fog detected", doc["data"]["reason"]);
  TEST_ASSERT_EQUAL(3, static_cast<int>(doc["data"].as<JsonObjectConst>().size()));
}

void test_event_texts_are_copied_into_the_message() {
  // ArduinoJson keeps only a pointer to a char array; the event must be free to disappear afterwards
  NodeEvent e = NodeEvent::cmdAck("c-17", AEROION_ACK_REJECTED, "state is LOCKED");
  JsonDocument doc;
  buildEventPayload(doc.to<JsonObject>(), kId, kTs, 9, e);
  strcpy(e.id, "changed");
  strcpy(e.text, "changed");
  TEST_ASSERT_EQUAL_STRING("c-17", doc["data"]["cmd_id"]);
  TEST_ASSERT_EQUAL_STRING("state is LOCKED", doc["data"]["reason"]);
}

void test_long_texts_are_cut_to_the_contract_limits() {
  char text[200];
  memset(text, 'x', sizeof text - 1);
  text[sizeof text - 1] = '\0';
  TEST_ASSERT_EQUAL(64, static_cast<int>(strlen(NodeEvent::stateChange(AEROION_STATE_LOCKED, AEROION_STATE_ARMED, text).text)));
  TEST_ASSERT_EQUAL(120, static_cast<int>(strlen(NodeEvent::faultEvent(AEROION_FAULT_OTHER, text).text)));
  TEST_ASSERT_EQUAL(36, static_cast<int>(strlen(NodeEvent::cmdAck(text, AEROION_ACK_ACCEPTED, "").id)));
}

void test_badge_tap_event() {
  JsonDocument doc = eventFor(NodeEvent::badgeTap("04A1B2C3", AEROION_ROLE_OPERATOR, AEROION_BADGE_ARM,
                                                  AEROION_BADGE_RESULT_GRANTED));
  TEST_ASSERT_EQUAL_STRING("BADGE_TAP", doc["type"]);
  TEST_ASSERT_EQUAL_STRING("04A1B2C3", doc["data"]["uid"]);
  TEST_ASSERT_EQUAL_STRING("operator", doc["data"]["role"]);
  TEST_ASSERT_EQUAL_STRING("arm", doc["data"]["action"]);
  TEST_ASSERT_EQUAL_STRING("granted", doc["data"]["result"]);
}

void test_arc_and_fog_events() {
  JsonDocument arc = eventFor(NodeEvent::arc(412.34f, 0.6234f, true));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 412.3f, arc["data"]["peak_ua"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.623f, arc["data"]["hv_level"].as<float>());
  TEST_ASSERT_TRUE(arc["data"]["predicted"].as<bool>());
  JsonDocument fog = eventFor(NodeEvent::fogDetected(0.3512f, AEROION_FOG_SOURCE_OPTICAL));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.351f, fog["data"]["fog_idx"].as<float>());
  TEST_ASSERT_EQUAL_STRING("optical", fog["data"]["source"]);
  JsonDocument cleared = eventFor(NodeEvent::fogCleared(30.46f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.5f, cleared["data"]["duration_s"].as<float>());
}

void test_maintenance_and_ack_events() {
  JsonDocument on = eventFor(NodeEvent::maintenance(true, "0E0E0E0E"));
  TEST_ASSERT_EQUAL_STRING("MAINT_ON", on["type"]);
  TEST_ASSERT_EQUAL_STRING("0E0E0E0E", on["data"]["uid"]);
  JsonDocument ack = eventFor(NodeEvent::cmdAck("c-17", AEROION_ACK_REJECTED, "state is LOCKED"));
  TEST_ASSERT_EQUAL_STRING("c-17", ack["data"]["cmd_id"]);
  TEST_ASSERT_EQUAL_STRING("rejected", ack["data"]["result"]);
  TEST_ASSERT_EQUAL_STRING("state is LOCKED", ack["data"]["reason"]);
}


int main() {
  UNITY_BEGIN();
  RUN_TEST(test_telemetry_has_every_contract_field);
  RUN_TEST(test_defaults_before_other_modules_exist);
  RUN_TEST(test_missing_sensors_are_null_not_zero);
  RUN_TEST(test_sensor_values_are_copied_and_rounded);
  RUN_TEST(test_nan_becomes_null);
  RUN_TEST(test_timestamp_keeps_all_64_bits);
  RUN_TEST(test_status_payload);
  RUN_TEST(test_info_payload_lists_sensors_and_position);
  RUN_TEST(test_write_line_wraps_topic_and_payload);
  RUN_TEST(test_write_line_refuses_a_too_small_buffer);
  RUN_TEST(test_state_change_event);
  RUN_TEST(test_event_texts_are_copied_into_the_message);
  RUN_TEST(test_long_texts_are_cut_to_the_contract_limits);
  RUN_TEST(test_badge_tap_event);
  RUN_TEST(test_arc_and_fog_events);
  RUN_TEST(test_maintenance_and_ack_events);
  return UNITY_END();
}