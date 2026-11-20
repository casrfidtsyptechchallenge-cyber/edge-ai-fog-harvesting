// Tests for FOG_ALERT, node to node (M7 part 3): the 16 bytes, and what the ESP32 gateway does when its MCX
// detects fog or a neighbour's alert comes in. They run on the PC.
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "fog_alert.h"
#include "node_gateway.h"

static const uint64_t T0 = 1791747040000ULL;

// The same bytes as Python's aeroion_contract.pack_fog_alert("n07", 1791747040123, 513, 0.5).
static const char* kPythonVector = "AE0101077B23722CA101000001028001";

static void toHex(const uint8_t* data, size_t n, char* out) {
  for (size_t i = 0; i < n; ++i) snprintf(out + 2 * i, 3, "%02X", data[i]);
}

static void fromHex(const char* hex, uint8_t* out) {
  for (size_t i = 0; i < strlen(hex) / 2; ++i) {
    unsigned v = 0;
    sscanf(hex + 2 * i, "%2x", &v);
    out[i] = static_cast<uint8_t>(v);
  }
}

// A fake ESP32: records the radio packets, the lines to the MCX and the log.
class RadioPort : public GatewayPort {
 public:
  uint8_t sent[8][kFogAlertSize];
  int sentCount = 0;
  char mcx[8][600];
  int mcxCount = 0;
  char lastLog[160] = "";
  int published = 0;

  bool mqttConnected() override { return true; }
  bool publish(const char*, const char*, bool) override {
    published++;
    return true;
  }
  void toMcx(const char* line) override {
    if (strncmp(line, "{\"link\":\"time\"", 14) == 0) return;  // time lines are not the subject here
    if (mcxCount < 8) snprintf(mcx[mcxCount++], sizeof mcx[0], "%s", line);
  }
  void log(const char* text) override { snprintf(lastLog, sizeof lastLog, "%s", text); }
  bool radioSend(const uint8_t* data, size_t length) override {
    TEST_ASSERT_EQUAL(kFogAlertSize, length);
    if (sentCount < 8) memcpy(sent[sentCount++], data, length);
    return true;
  }
};

static RadioPort port;
static uint32_t now;

static GatewayConfig config(const char* nodeId = "n01") {
  GatewayConfig c;
  c.nodeId = nodeId;
  c.fwVersion = "0.5.0-esp";
  c.sensorNames = nullptr;
  c.sensorCount = 0;
  return c;
}

void setUp() {
  port = RadioPort();
  now = 1000;
}
void tearDown() {}

static void mcxFog(NodeGateway& g, float fog, const char* source = "optical", uint64_t ts = T0) {
  char line[320];
  snprintf(line, sizeof line,
           "{\"topic\":\"aeroion/n01/event\",\"retain\":false,\"payload\":{\"schema_version\":\"1.1\",\"node_id\":\"n01\","
           "\"ts\":%llu,\"seq\":4,\"type\":\"FOG_DETECTED\",\"data\":{\"fog_idx\":%.3f,\"source\":\"%s\"}}}",
           static_cast<unsigned long long>(ts), fog, source);
  g.fromMcx(line, T0, now);
}

static void mcxTelemetry(NodeGateway& g) {  // any line: the MCX is there
  g.fromMcx("{\"topic\":\"aeroion/n01/status\",\"retain\":true,\"payload\":{\"schema_version\":\"1.1\",\"node_id\":\"n01\","
            "\"online\":true}}",
            T0, now);
}

static void neighbour(NodeGateway& g, int node, uint16_t seq, uint64_t ts, bool synced = true, uint64_t unixNow = T0 + 2000) {
  FogAlert a = {node, ts, seq, 0.6f, synced, false};
  uint8_t packet[kFogAlertSize];
  TEST_ASSERT_TRUE(packFogAlert(a, packet));
  g.fromRadio(packet, sizeof packet, unixNow, true, now);
}

// ---------------------------------------------------------------- the 16 bytes

void test_the_bytes_are_the_same_as_python() {
  FogAlert a = {7, 1791747040123ULL, 513, 0.5f, true, false};
  uint8_t packet[kFogAlertSize];
  char hex[2 * kFogAlertSize + 1];
  TEST_ASSERT_TRUE(packFogAlert(a, packet));
  toHex(packet, sizeof packet, hex);
  TEST_ASSERT_EQUAL_STRING(kPythonVector, hex);
}

void test_an_alert_reads_back() {
  uint8_t packet[kFogAlertSize];
  fromHex("AE0101020023722CA101000005009903", packet);  // Python: n02, seq 5, fog 0.6, synced, virtual
  FogAlert a;
  TEST_ASSERT_EQUAL(kAlertOk, readFogAlert(packet, sizeof packet, &a));
  TEST_ASSERT_EQUAL(2, a.nodeNum);
  TEST_ASSERT_TRUE(a.tsMs == T0);
  TEST_ASSERT_EQUAL_UINT16(5, a.seq);
  TEST_ASSERT_FLOAT_WITHIN(0.003f, 0.6f, a.fogIdx);
  TEST_ASSERT_TRUE(a.timeSynced);
  TEST_ASSERT_TRUE(a.virtualSource);
}

void test_other_packets_are_recognised() {
  uint8_t packet[kFogAlertSize];
  FogAlert a;
  fromHex(kPythonVector, packet);
  TEST_ASSERT_EQUAL(kAlertWrongSize, readFogAlert(packet, 15, &a));
  packet[0] = 0x42;
  TEST_ASSERT_EQUAL(kAlertForeign, readFogAlert(packet, sizeof packet, &a));
  packet[0] = 0xAE;
  packet[1] = 2;
  TEST_ASSERT_EQUAL(kAlertUnsupported, readFogAlert(packet, sizeof packet, &a));
  packet[1] = 1;
  packet[3] = 0;
  TEST_ASSERT_EQUAL(kAlertBadNode, readFogAlert(packet, sizeof packet, &a));
  FogAlert bad = {100, T0, 1, 0.5f, true, false};
  TEST_ASSERT_FALSE(packFogAlert(bad, packet));
}

void test_fog_is_kept_between_0_and_1() {
  uint8_t packet[kFogAlertSize];
  FogAlert a = {1, T0, 1, 1.7f, true, false};
  packFogAlert(a, packet);
  TEST_ASSERT_EQUAL_UINT8(255, packet[14]);
  a.fogIdx = -0.2f;
  packFogAlert(a, packet);
  TEST_ASSERT_EQUAL_UINT8(0, packet[14]);
}

void test_node_numbers() {
  TEST_ASSERT_EQUAL(7, nodeNumberOf("n07"));
  TEST_ASSERT_EQUAL(99, nodeNumberOf("n99"));
  TEST_ASSERT_EQUAL(-1, nodeNumberOf("n00"));
  TEST_ASSERT_EQUAL(-1, nodeNumberOf("n7"));
  TEST_ASSERT_EQUAL(-1, nodeNumberOf("x07"));
  TEST_ASSERT_EQUAL(-1, nodeNumberOf(nullptr));
}

void test_each_alert_counts_once() {
  FogAlertFilter filter;
  FogAlert a = {2, T0, 5, 0.6f, true, false};
  TEST_ASSERT_TRUE(filter.isNew(a));
  TEST_ASSERT_FALSE(filter.isNew(a));  // the second and third copies
  TEST_ASSERT_FALSE(filter.isNew(a));
  a.seq = 6;
  TEST_ASSERT_TRUE(filter.isNew(a));
  FogAlert other = {3, T0, 6, 0.6f, true, false};
  TEST_ASSERT_TRUE(filter.isNew(other));  // another node, same seq
  FogAlert restarted = {2, T0 + 90000, 6, 0.6f, true, false};
  TEST_ASSERT_TRUE(filter.isNew(restarted));  // same seq after a restart, but a new time
}

// ---------------------------------------------------------------- sending

void test_fog_at_this_node_goes_to_the_neighbours_three_times() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  mcxFog(g, 0.5f, "optical", T0 - 3600000ULL);  // the MCX's clock is not set yet
  TEST_ASSERT_EQUAL(1, port.sentCount);  // the first copy at once
  now += 99;
  g.poll(T0, true, now);
  TEST_ASSERT_EQUAL(1, port.sentCount);
  now += 1;
  g.poll(T0, true, now);
  now += 100;
  g.poll(T0, true, now);
  now += 100;
  g.poll(T0, true, now);
  TEST_ASSERT_EQUAL(3, port.sentCount);
  TEST_ASSERT_EQUAL_MEMORY(port.sent[0], port.sent[2], kFogAlertSize);  // copies are identical
  FogAlert a;
  TEST_ASSERT_EQUAL(kAlertOk, readFogAlert(port.sent[0], kFogAlertSize, &a));
  TEST_ASSERT_EQUAL(1, a.nodeNum);
  TEST_ASSERT_TRUE(a.tsMs == T0);  // the ESP32's synced clock when the event came, not the MCX's
  TEST_ASSERT_EQUAL_UINT16(1, a.seq);
  TEST_ASSERT_EQUAL_UINT8(128, port.sent[0][14]);
  TEST_ASSERT_TRUE(a.timeSynced);
  TEST_ASSERT_FALSE(a.virtualSource);
  TEST_ASSERT_EQUAL(1, g.alertsSent());
  TEST_ASSERT_EQUAL(1, port.published);  // the FOG_DETECTED event itself still goes to the station
}

void test_at_most_one_alert_per_10_s() {
  NodeGateway g(config(), port);
  g.poll(T0, false, now);
  mcxFog(g, 0.5f, "virtual");
  now += 9999;
  mcxFog(g, 0.6f);
  TEST_ASSERT_EQUAL(1, g.alertsSent());
  now += 1;
  mcxFog(g, 0.6f);
  TEST_ASSERT_EQUAL(2, g.alertsSent());
  FogAlert first, second;
  readFogAlert(port.sent[0], kFogAlertSize, &first);
  readFogAlert(port.sent[1], kFogAlertSize, &second);
  TEST_ASSERT_TRUE(first.virtualSource);
  TEST_ASSERT_FALSE(first.timeSynced);  // the ESP32 had no time yet
  TEST_ASSERT_EQUAL_UINT16(2, second.seq);
}

// ---------------------------------------------------------------- receiving

void test_a_neighbours_alert_pre_arms_this_node() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  mcxTelemetry(g);
  neighbour(g, 2, 5, T0 + 500);
  TEST_ASSERT_EQUAL(1, port.mcxCount);
  JsonDocument line;
  TEST_ASSERT_FALSE(deserializeJson(line, port.mcx[0]));
  TEST_ASSERT_EQUAL_STRING("aeroion/n01/cmd", line["topic"]);
  JsonObject cmd = line["payload"];
  TEST_ASSERT_EQUAL_STRING("PREARM", cmd["type"]);
  TEST_ASSERT_EQUAL_STRING("fa-n02-5", cmd["cmd_id"]);
  TEST_ASSERT_EQUAL_STRING(AEROION_SCHEMA_VERSION, cmd["schema_version"]);
  TEST_ASSERT_TRUE(cmd["ts"].as<uint64_t>() == T0 + 500);  // eta counts from the neighbour's detection
  TEST_ASSERT_EQUAL(60, cmd["ttl_s"].as<int>());  // as long as an alert is taken (alertMaxAgeMs)
  TEST_ASSERT_EQUAL_FLOAT(60.0f, cmd["params"]["eta_s"].as<float>());
  TEST_ASSERT_EQUAL_STRING("n02", cmd["params"]["source_node"]);
  TEST_ASSERT_EQUAL(1, g.alertsHeard());

  neighbour(g, 2, 5, T0 + 500);  // the second copy
  TEST_ASSERT_EQUAL(1, port.mcxCount);
  TEST_ASSERT_EQUAL(1, g.alertsHeard());
}

void test_own_stale_and_foreign_packets_change_nothing() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  mcxTelemetry(g);
  neighbour(g, 1, 1, T0);  // our own node number
  neighbour(g, 3, 1, T0, true, T0 + 60001);  // 60 s old
  uint8_t other[12] = {0};
  g.fromRadio(other, sizeof other, T0, true, now);  // someone else's ESP-NOW traffic
  TEST_ASSERT_EQUAL(0, port.mcxCount);
  neighbour(g, 4, 1, T0 - 7200000ULL, false);  // the sender had no time: its age cannot be judged
  TEST_ASSERT_EQUAL(1, port.mcxCount);
  JsonDocument line;
  deserializeJson(line, port.mcx[0]);
  TEST_ASSERT_TRUE(line["payload"]["ts"].as<uint64_t>() == T0 + 2000);  // so eta counts from now
}

void test_no_prearm_while_the_mcx_is_silent() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  neighbour(g, 2, 1, T0);
  TEST_ASSERT_EQUAL(0, port.mcxCount);
  TEST_ASSERT_NOT_NULL(strstr(port.lastLog, "silent"));
}

void test_at_most_one_prearm_per_30_s() {
  NodeGateway g(config(), port);
  g.poll(T0, true, now);
  mcxTelemetry(g);
  neighbour(g, 2, 1, T0);
  now += 29999;
  mcxTelemetry(g);
  neighbour(g, 3, 1, T0 + 29000, true, T0 + 30000);
  TEST_ASSERT_EQUAL(1, port.mcxCount);
  now += 1;
  neighbour(g, 3, 2, T0 + 29500, true, T0 + 30000);
  TEST_ASSERT_EQUAL(2, port.mcxCount);
  TEST_ASSERT_EQUAL(3, g.alertsHeard());
}

// ---------------------------------------------------------------- Wokwi: radio packets on the link

void test_on_the_link_in_wokwi() {
  GatewayConfig c = config();
  c.radioOnLink = true;
  NodeGateway g(c, port);
  g.poll(T0, true, now);
  mcxFog(g, 0.5f);
  TEST_ASSERT_EQUAL(1, port.mcxCount);  // the sent alert, shown on the link
  JsonDocument shown;
  deserializeJson(shown, port.mcx[0]);
  TEST_ASSERT_EQUAL_STRING("radio", shown["link"]);
  char hex[2 * kFogAlertSize + 1];
  toHex(port.sent[0], kFogAlertSize, hex);
  TEST_ASSERT_EQUAL_STRING(hex, shown["hex"]);

  g.fromMcx("{\"link\":\"radio\",\"hex\":\"AE0101020023722CA101000005009903\"}", T0 + 1000, now);  // from "n02"
  TEST_ASSERT_EQUAL(2, port.mcxCount);
  TEST_ASSERT_NOT_NULL(strstr(port.mcx[1], "\"cmd_id\":\"fa-n02-5\""));

  NodeGateway plain(config(), port);  // a real node takes no radio packets from the MCX's wire
  plain.fromMcx("{\"link\":\"radio\",\"hex\":\"AE0101020023722CA101000005009904\"}", T0 + 1000, now);
  TEST_ASSERT_EQUAL(0, plain.alertsHeard());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_bytes_are_the_same_as_python);
  RUN_TEST(test_an_alert_reads_back);
  RUN_TEST(test_other_packets_are_recognised);
  RUN_TEST(test_fog_is_kept_between_0_and_1);
  RUN_TEST(test_node_numbers);
  RUN_TEST(test_each_alert_counts_once);
  RUN_TEST(test_fog_at_this_node_goes_to_the_neighbours_three_times);
  RUN_TEST(test_at_most_one_alert_per_10_s);
  RUN_TEST(test_a_neighbours_alert_pre_arms_this_node);
  RUN_TEST(test_own_stale_and_foreign_packets_change_nothing);
  RUN_TEST(test_no_prearm_while_the_mcx_is_silent);
  RUN_TEST(test_at_most_one_prearm_per_30_s);
  RUN_TEST(test_on_the_link_in_wokwi);
  return UNITY_END();
}