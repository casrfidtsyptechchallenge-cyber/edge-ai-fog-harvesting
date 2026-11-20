// Tests for the ESP32 side of the station link (lib/gateway, M7 part 2): what reaches the broker, what
// reaches the MCX, offline buffering and the answers the ESP32 gives itself. They run on the PC.
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "node_gateway.h"
#include "offline_buffer.h"

static const uint64_t T0 = 1791747040000ULL;

// A fake ESP32: records everything the gateway sends.
class FakePort : public GatewayPort {
 public:
  bool connected = true;
  bool publishWorks = true;
  struct Message {
    char topic[48];
    char payload[1200];
    bool retain;
  };
  Message published[160];
  int count = 0;
  char mcx[16][4200];  // commands to the MCX
  int mcxCount = 0;
  char lastTime[64] = "";  // time lines to the MCX
  int timeLines = 0;
  char lastLog[160] = "";

  bool mqttConnected() override { return connected; }
  bool publish(const char* topic, const char* payload, bool retain) override {
    if (!connected || !publishWorks) return false;
    if (count < 160) {
      snprintf(published[count].topic, sizeof published[count].topic, "%s", topic);
      snprintf(published[count].payload, sizeof published[count].payload, "%s", payload);
      published[count].retain = retain;
      count++;
    }
    return true;
  }
  void toMcx(const char* line) override {
    if (strncmp(line, "{\"link\":\"time\"", 14) == 0) {
      snprintf(lastTime, sizeof lastTime, "%s", line);
      timeLines++;
    } else if (mcxCount < 16) {
      snprintf(mcx[mcxCount++], sizeof mcx[0], "%s", line);
    }
  }
  void log(const char* text) override { snprintf(lastLog, sizeof lastLog, "%s", text); }

  // The JSON of message i (negative: from the end).
  JsonDocument json(int i) const {
    JsonDocument doc;
    deserializeJson(doc, published[i < 0 ? count + i : i].payload);
    return doc;
  }
  int countTopic(const char* topic) const {
    int n = 0;
    for (int i = 0; i < count; ++i) n += strcmp(published[i].topic, topic) == 0;
    return n;
  }
};

static FakePort port;
static uint32_t now;
static int tares;
static bool tareWithLoadCell() {
  tares++;
  return true;
}
static bool noLoadCell() { return false; }

static const char* kEspSensors[] = {"SHT45", "HX711"};

static GatewayConfig config() {
  GatewayConfig c;
  c.nodeId = "n01";
  c.fwVersion = "0.5.0-esp";
  c.sensorNames = kEspSensors;
  c.sensorCount = 2;
  return c;
}

void setUp() {
  port = FakePort();
  now = 1000;
  tares = 0;
}
void tearDown() {}

// MCX lines, as main.cpp on the MCX writes them.
static const char* kTelemetry =
    "{\"topic\":\"aeroion/n01/telemetry\",\"retain\":false,\"payload\":{\"schema_version\":\"1.1\",\"node_id\":\"n01\","
    "\"ts\":1791747041000,\"seq\":7,\"state\":\"HARVEST\",\"hv_on\":true,\"hv_level\":0.4,\"fog_idx_opt\":0.5,"
    "\"fog_idx_virt\":null,\"t_c\":null,\"rh_pct\":null,\"mass_g\":null,\"rate_gpm\":null,\"i_ua\":12.5,\"p_w\":null,"
    "\"e_wh\":null,\"arc_risk\":null,\"health\":{\"onset_level\":null,\"k_coef\":null,\"score\":null},"
    "\"radar_presence\":false,\"lid_closed\":true,\"batt_pct\":null}}";

static void mcxEvent(char* out, size_t cap, const char* type, const char* data) {
  snprintf(out, cap,
           "{\"topic\":\"aeroion/n01/event\",\"retain\":false,\"payload\":{\"schema_version\":\"1.1\",\"node_id\":\"n01\","
           "\"ts\":1791747041000,\"seq\":3,\"type\":\"%s\",\"data\":%s}}",
           type, data);
}

static void command(char* out, size_t cap, const char* cmdId, const char* type, const char* params, uint64_t ts = T0,
                    int ttl = 30) {
  snprintf(out, cap, "{\"schema_version\":\"1.1\",\"cmd_id\":\"%s\",\"ts\":%llu,\"ttl_s\":%d,\"type\":\"%s\",\"params\":%s}",
           cmdId, static_cast<unsigned long long>(ts), ttl, type, params);
}

static void broker(NodeGateway& g, const char* topic, const char* payload, uint64_t unixMs = T0 + 500,
                   bool synced = true) {
  g.fromBroker(topic, payload, strlen(payload), unixMs, synced, now);
}

// ---------------------------------------------------------------- the offline buffer

void test_the_buffer_gives_back_the_oldest_first() {
  static OfflineBuffer b;
  b.clear();
  TEST_ASSERT_NULL(b.frontPayload());
  b.push("{\"a\":1}", true);
  b.push("{\"a\":2}", false);
  TEST_ASSERT_EQUAL(2, b.count());
  TEST_ASSERT_EQUAL_STRING("{\"a\":1}", b.frontPayload());
  TEST_ASSERT_TRUE(b.frontIsEvent());
  b.pop();
  TEST_ASSERT_EQUAL_STRING("{\"a\":2}", b.frontPayload());
  TEST_ASSERT_FALSE(b.frontIsEvent());
}

void test_a_full_buffer_drops_the_oldest_telemetry_first() {
  static OfflineBuffer b;
  char text[32];
  b.push("{\"event\":0}", true);
  b.push("{\"telemetry\":1}", false);
  for (int i = 2; i < OfflineBuffer::kSlots; ++i) {
    snprintf(text, sizeof text, "{\"event\":%d}", i);
    b.push(text, true);
  }
  TEST_ASSERT_EQUAL(OfflineBuffer::kSlots, b.count());
  b.push("{\"event\":99}", true);  // full: the telemetry goes, not the oldest event
  TEST_ASSERT_EQUAL(OfflineBuffer::kSlots, b.count());
  TEST_ASSERT_EQUAL_UINT32(1, b.dropped());
  TEST_ASSERT_EQUAL_STRING("{\"event\":0}", b.frontPayload());
  b.pop();
  TEST_ASSERT_EQUAL_STRING("{\"event\":2}", b.frontPayload());
  b.push("{\"event\":100}", true);  // full again
  b.push("{\"event\":101}", true);  // only events left: the oldest event goes
  TEST_ASSERT_EQUAL_STRING("{\"event\":3}", b.frontPayload());
  char tooLong[OfflineBuffer::kMaxPayload + 10];
  memset(tooLong, 'x', sizeof tooLong - 1);
  tooLong[sizeof tooLong - 1] = '\0';
  TEST_ASSERT_FALSE(b.push(tooLong, true));
}

// ---------------------------------------------------------------- MCX -> broker

void test_telemetry_gets_the_esp32_values_and_the_node_seq() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  EspReadings r;
  r.tempC = 14.236f;
  r.rhPct = 97.04f;
  r.massG = 12.5f;
  r.rateGpm = 0.4f;
  g.setReadings(r);
  g.fromMcx(kTelemetry, T0, now);
  TEST_ASSERT_EQUAL(1, port.count);
  TEST_ASSERT_EQUAL_STRING("aeroion/n01/telemetry", port.published[0].topic);
  TEST_ASSERT_FALSE(port.published[0].retain);
  JsonDocument t = port.json(0);
  TEST_ASSERT_EQUAL_FLOAT(14.24f, t["t_c"].as<float>());
  TEST_ASSERT_EQUAL_FLOAT(97.0f, t["rh_pct"].as<float>());
  TEST_ASSERT_EQUAL_FLOAT(12.5f, t["mass_g"].as<float>());
  TEST_ASSERT_TRUE(t["fog_idx_virt"].isNull());  // the ESP32 has no value: the MCX's null stays
  TEST_ASSERT_EQUAL_FLOAT(12.5f, t["i_ua"].as<float>());  // the MCX's values are kept
  TEST_ASSERT_EQUAL(1, t["seq"].as<int>());                // numbered by the ESP32
  TEST_ASSERT_FALSE(t["buffered"].is<bool>());
}

void test_one_seq_for_everything_the_node_sends() {
  NodeGateway g(config(), port);
  g.setTare(tareWithLoadCell);
  g.poll(T0, true, now);
  char line[512], cmd[256];
  mcxEvent(line, sizeof line, "STATE_CHANGE", "{\"from\":\"LOCKED\",\"to\":\"ARMED\",\"reason\":\"operator badge\"}");
  g.fromMcx(line, T0, now);
  command(cmd, sizeof cmd, "c-tare", "TARE", "{}");
  broker(g, "aeroion/n01/cmd", cmd);  // answered by the ESP32
  g.fromMcx(kTelemetry, T0, now);
  TEST_ASSERT_EQUAL(3, port.count);
  for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(i + 1, port.json(i)["seq"].as<int>());
}

void test_info_and_status_carry_both_chips() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  EspReadings r;
  r.rssiDbm = -61.4f;
  g.setReadings(r);
  g.fromMcx("{\"topic\":\"aeroion/n01/info\",\"retain\":true,\"payload\":{\"schema_version\":\"1.1\",\"node_id\":\"n01\","
            "\"ts\":1791747041000,\"fw\":\"0.4.0-mcx\",\"models\":{\"arc\":null,\"fog\":null,\"health\":null},"
            "\"position\":{\"x_m\":0,\"y_m\":0},\"sensors\":[\"COLLECTOR-I\",\"LD2410\"]}}",
            T0, now);
  g.fromMcx("{\"topic\":\"aeroion/n01/status\",\"retain\":true,\"payload\":{\"schema_version\":\"1.1\",\"node_id\":\"n01\","
            "\"online\":true,\"ts\":1791747041000,\"uptime_s\":5,\"time_synced\":true,\"buffered_msgs\":0}}",
            T0, now);
  TEST_ASSERT_EQUAL(2, port.count);
  TEST_ASSERT_TRUE(port.published[0].retain && port.published[1].retain);
  JsonDocument info = port.json(0);
  TEST_ASSERT_EQUAL_STRING("0.4.0-mcx+0.5.0-esp", info["fw"]);
  TEST_ASSERT_EQUAL(4, info["sensors"].size());
  TEST_ASSERT_EQUAL_STRING("HX711", info["sensors"][3]);
  JsonDocument status = port.json(1);
  TEST_ASSERT_EQUAL(-61, status["rssi_dbm"].as<int>());
  TEST_ASSERT_EQUAL(0, status["buffered_msgs"].as<int>());
}

void test_offline_events_are_kept_and_sent_later_marked_buffered() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  port.connected = false;
  g.poll(T0, true, now);  // the broker is lost: FAULT COMM_LOST, kept
  char line[512];
  mcxEvent(line, sizeof line, "FOG_DETECTED", "{\"fog_idx\":0.5,\"source\":\"optical\"}");
  g.fromMcx(line, T0, now);
  for (int i = 0; i < 61; ++i) g.fromMcx(kTelemetry, T0, now);  // one in 30 is kept: 3
  g.fromMcx("{\"topic\":\"aeroion/n01/status\",\"retain\":true,\"payload\":{\"schema_version\":\"1.1\",\"node_id\":\"n01\","
            "\"online\":true,\"ts\":1791747041000,\"uptime_s\":5,\"time_synced\":true,\"buffered_msgs\":0}}",
            T0, now);
  TEST_ASSERT_EQUAL(0, port.count);
  TEST_ASSERT_EQUAL(5, g.buffered());

  port.connected = true;
  g.poll(T0, true, now);  // status again at once (retained)...
  TEST_ASSERT_EQUAL(1, port.count);
  g.poll(T0, true, now + 1999);
  TEST_ASSERT_EQUAL(1, port.count);
  g.poll(T0, true, now + 2000);  // ...the kept messages 2 s later, when the station has subscribed again
  TEST_ASSERT_EQUAL(6, port.count);
  TEST_ASSERT_EQUAL_STRING("aeroion/n01/status", port.published[0].topic);
  TEST_ASSERT_EQUAL(5, port.json(0)["buffered_msgs"].as<int>());
  JsonDocument fault = port.json(1);
  TEST_ASSERT_EQUAL_STRING("FAULT", fault["type"]);
  TEST_ASSERT_EQUAL_STRING("COMM_LOST", fault["data"]["code"]);
  TEST_ASSERT_TRUE(fault["buffered"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("FOG_DETECTED", port.json(2)["type"]);
  TEST_ASSERT_EQUAL_STRING("aeroion/n01/telemetry", port.published[3].topic);
  TEST_ASSERT_TRUE(port.json(3)["buffered"].as<bool>());
  TEST_ASSERT_EQUAL(0, g.buffered());

  g.fromMcx(kTelemetry, T0, now);  // live again: not marked
  TEST_ASSERT_FALSE(port.json(-1)["buffered"].is<bool>());
}

void test_a_long_backlog_goes_out_a_few_messages_per_loop() {
  NodeGateway g(config(), port);
  port.connected = false;
  char line[512];
  mcxEvent(line, sizeof line, "FOG_CLEARED", "{\"duration_s\":12}");
  for (int i = 0; i < 20; ++i) g.fromMcx(line, T0, now);
  port.connected = true;
  g.poll(T0, true, now);
  TEST_ASSERT_EQUAL(0, port.count);
  g.poll(T0, true, now + 2000);
  TEST_ASSERT_EQUAL(8, port.count);
  g.poll(T0, true, now + 2010);
  g.poll(T0, true, now + 2020);
  TEST_ASSERT_EQUAL(20, port.count);
}

void test_a_failed_publish_is_kept_too() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  port.publishWorks = false;
  char line[512];
  mcxEvent(line, sizeof line, "FOG_CLEARED", "{\"duration_s\":12}");
  g.fromMcx(line, T0, now);
  TEST_ASSERT_EQUAL(1, g.buffered());
  port.publishWorks = true;
  g.poll(T0, true, now + 2000);
  TEST_ASSERT_EQUAL(1, port.count);
  TEST_ASSERT_TRUE(port.json(0)["buffered"].as<bool>());
}

// ---------------------------------------------------------------- broker -> MCX

void test_a_command_goes_to_the_mcx_and_its_answer_to_the_broker() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  g.fromMcx(kTelemetry, T0, now);  // the MCX is there
  char cmd[256], line[512];
  command(cmd, sizeof cmd, "c-lock", "LOCK", "{\"reason\":\"test\"}");
  broker(g, "aeroion/all/cmd", cmd);
  TEST_ASSERT_EQUAL(1, port.mcxCount);
  JsonDocument sent;
  TEST_ASSERT_FALSE(deserializeJson(sent, port.mcx[0]));
  TEST_ASSERT_EQUAL_STRING("aeroion/all/cmd", sent["topic"]);
  TEST_ASSERT_EQUAL_STRING("c-lock", sent["payload"]["cmd_id"]);
  TEST_ASSERT_EQUAL_STRING("test", sent["payload"]["params"]["reason"]);

  mcxEvent(line, sizeof line, "CMD_ACK", "{\"cmd_id\":\"c-lock\",\"result\":\"accepted\",\"reason\":\"\"}");
  now += 500;
  g.fromMcx(line, T0, now);
  now += 5000;
  g.fromMcx(kTelemetry, T0, now);
  g.poll(T0 + 5500, true, now);
  int acks = 0;
  for (int i = 0; i < port.count; ++i) acks += port.json(i)["type"] == "CMD_ACK";
  TEST_ASSERT_EQUAL(1, acks);  // the MCX's answer only
}

void test_the_esp32_answers_when_the_mcx_does_not() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  g.fromMcx(kTelemetry, T0, now);
  char cmd[256];
  command(cmd, sizeof cmd, "c-pre", "PREARM", "{\"eta_s\":20,\"source_node\":\"n02\"}");
  broker(g, "aeroion/n01/cmd", cmd);
  now += 2999;
  g.fromMcx(kTelemetry, T0, now);
  g.poll(T0, true, now);
  TEST_ASSERT_FALSE(port.json(-1)["type"] == "CMD_ACK");
  now += 1;
  g.poll(T0, true, now);
  JsonDocument ack = port.json(-1);
  TEST_ASSERT_EQUAL_STRING("CMD_ACK", ack["type"]);
  TEST_ASSERT_EQUAL_STRING("c-pre", ack["data"]["cmd_id"]);
  TEST_ASSERT_EQUAL_STRING("rejected", ack["data"]["result"]);
  TEST_ASSERT_NOT_NULL(strstr(ack["data"]["reason"].as<const char*>(), "no answer"));
}

void test_commands_are_refused_while_the_mcx_is_silent() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  char cmd[256];
  command(cmd, sizeof cmd, "c-1", "LOCK", "{\"reason\":\"x\"}");
  broker(g, "aeroion/n01/cmd", cmd);  // never heard from the MCX
  TEST_ASSERT_EQUAL(0, port.mcxCount);
  TEST_ASSERT_EQUAL_STRING("rejected", port.json(-1)["data"]["result"]);

  g.fromMcx(kTelemetry, T0, now);
  now += 4999;
  g.poll(T0, true, now);
  TEST_ASSERT_TRUE(g.mcxAlive());
  now += 1;
  g.poll(T0, true, now);  // 5 s of silence
  g.poll(T0, true, now + 10000);
  TEST_ASSERT_FALSE(g.mcxAlive());
  int faults = 0;
  for (int i = 0; i < port.count; ++i) faults += port.json(i)["type"] == "FAULT";
  TEST_ASSERT_EQUAL(1, faults);
  g.fromMcx(kTelemetry, T0, now + 11000);
  TEST_ASSERT_TRUE(g.mcxAlive());
}

void test_tare_is_done_by_the_esp32() {
  NodeGateway g(config(), port);
  g.setTare(tareWithLoadCell);
  g.poll(T0, true, now);
  char cmd[256];
  command(cmd, sizeof cmd, "c-tare", "TARE", "{}");
  broker(g, "aeroion/n01/cmd", cmd);
  TEST_ASSERT_EQUAL(1, tares);
  TEST_ASSERT_EQUAL(0, port.mcxCount);  // not for the MCX
  TEST_ASSERT_EQUAL_STRING("accepted", port.json(-1)["data"]["result"]);
  TEST_ASSERT_EQUAL_STRING("", port.json(-1)["data"]["reason"]);

  command(cmd, sizeof cmd, "c-tare-old", "TARE", "{}", T0 - 60000, 30);
  broker(g, "aeroion/n01/cmd", cmd);
  TEST_ASSERT_EQUAL_STRING("expired", port.json(-1)["data"]["result"]);
  TEST_ASSERT_EQUAL(1, tares);

  NodeGateway bare(config(), port);
  bare.setTare(noLoadCell);
  bare.poll(T0, true, now);
  command(cmd, sizeof cmd, "c-tare-2", "TARE", "{}");
  broker(bare, "aeroion/n01/cmd", cmd);
  TEST_ASSERT_EQUAL_STRING("rejected", port.json(-1)["data"]["result"]);
}

void test_what_is_not_a_command_is_ignored_and_a_huge_one_refused() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  g.fromMcx(kTelemetry, T0, now);
  int before = port.count;
  broker(g, "aeroion/n01/cmd", "LOCK now");
  broker(g, "aeroion/n01/cmd", "[1,2]");
  TEST_ASSERT_EQUAL(before, port.count);
  TEST_ASSERT_EQUAL(0, port.mcxCount);

  static char big[6000];
  static char params[5000];
  snprintf(params, sizeof params, "{\"reason\":\"");
  memset(params + 11, 'x', 4200);
  snprintf(params + 4211, sizeof params - 4211, "\"}");
  command(big, sizeof big, "c-big", "LOCK", params);
  broker(g, "aeroion/n01/cmd", big);
  TEST_ASSERT_EQUAL(0, port.mcxCount);
  TEST_ASSERT_EQUAL_STRING("rejected", port.json(-1)["data"]["result"]);
}

void test_times_read_a_moment_apart_are_not_silence() {
  // main_link.cpp reads millis() once per loop for poll(), and again when a line or a command arrives.
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  g.fromMcx(kTelemetry, T0, now + 3);
  char cmd[256];
  command(cmd, sizeof cmd, "c-lock", "LOCK", "{\"reason\":\"x\"}");
  g.fromBroker("aeroion/n01/cmd", cmd, strlen(cmd), T0 + 500, true, now + 5);
  g.poll(T0, true, now);  // earlier than both
  TEST_ASSERT_TRUE(g.mcxAlive());
  for (int i = 0; i < port.count; ++i) TEST_ASSERT_FALSE(port.json(i)["type"] == "CMD_ACK");
  TEST_ASSERT_EQUAL(1, port.mcxCount);
}

// ---------------------------------------------------------------- time, will

void test_the_time_goes_to_the_mcx_only_once_the_esp32_has_it() {
  NodeGateway g(config(), port);
  g.poll(T0, false, now);
  TEST_ASSERT_EQUAL(0, port.timeLines);
  g.poll(T0, true, now);
  TEST_ASSERT_EQUAL(1, port.timeLines);
  TEST_ASSERT_EQUAL_STRING("{\"link\":\"time\",\"ts\":1791747040000}", port.lastTime);
  g.poll(T0 + 4999, true, now + 4999);
  TEST_ASSERT_EQUAL(1, port.timeLines);
  g.poll(T0 + 5000, true, now + 5000);
  TEST_ASSERT_EQUAL(2, port.timeLines);
  TEST_ASSERT_EQUAL(0, port.mcxCount);
}

void test_the_last_will_says_offline() {
  NodeGateway g(config(), port);
  char will[96];
  g.lastWill(will, sizeof will);
  TEST_ASSERT_EQUAL_STRING("{\"schema_version\":\"" AEROION_SCHEMA_VERSION "\",\"node_id\":\"n01\",\"online\":false}", will);
  TEST_ASSERT_EQUAL_STRING("aeroion/n01/status", g.statusTopic());
  TEST_ASSERT_EQUAL_STRING("aeroion/n01/cmd", g.cmdTopic());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_buffer_gives_back_the_oldest_first);
  RUN_TEST(test_a_full_buffer_drops_the_oldest_telemetry_first);
  RUN_TEST(test_telemetry_gets_the_esp32_values_and_the_node_seq);
  RUN_TEST(test_one_seq_for_everything_the_node_sends);
  RUN_TEST(test_info_and_status_carry_both_chips);
  RUN_TEST(test_offline_events_are_kept_and_sent_later_marked_buffered);
  RUN_TEST(test_a_long_backlog_goes_out_a_few_messages_per_loop);
  RUN_TEST(test_a_failed_publish_is_kept_too);
  RUN_TEST(test_a_command_goes_to_the_mcx_and_its_answer_to_the_broker);
  RUN_TEST(test_the_esp32_answers_when_the_mcx_does_not);
  RUN_TEST(test_commands_are_refused_while_the_mcx_is_silent);
  RUN_TEST(test_tare_is_done_by_the_esp32);
  RUN_TEST(test_what_is_not_a_command_is_ignored_and_a_huge_one_refused);
  RUN_TEST(test_times_read_a_moment_apart_are_not_silence);
  RUN_TEST(test_the_time_goes_to_the_mcx_only_once_the_esp32_has_it);
  RUN_TEST(test_the_last_will_says_offline);
  return UNITY_END();
}