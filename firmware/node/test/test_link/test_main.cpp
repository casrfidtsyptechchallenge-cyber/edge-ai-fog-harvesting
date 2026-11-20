// Tests for the link to the station (lib/link, M7): lines, the clock, the commands and their answers.
// They run on the PC: pio test -e native
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "badge_list.h"
#include "command_handler.h"
#include "line_reader.h"
#include "node_link.h"
#include "safety_core.h"
#include "unix_clock.h"

static const uint64_t T0 = 1791747040000ULL;  // Unix ms of the station when the tests start
static const char* kOperator = "0A0B0C0D";
static const char* kTechnician = "0E0E0E0E";

// Keeps every event so tests can check what was sent.
class RecordingSink : public EventSink {
 public:
  NodeEvent events[32];
  int count = 0;

  void emit(const NodeEvent& event) override {
    if (count < 32) events[count++] = event;
  }
  int countOf(aeroion_event_t type) const {
    int n = 0;
    for (int i = 0; i < count; ++i) n += events[i].type == type;
    return n;
  }
  const NodeEvent* lastOf(aeroion_event_t type) const {
    for (int i = count - 1; i >= 0; --i) {
      if (events[i].type == type) return &events[i];
    }
    return nullptr;
  }
  void clear() { count = 0; }
};

static RecordingSink sink;
static uint32_t now;

void setUp() {
  sink.clear();
  now = 1000;
}
void tearDown() {}

static SafetyConfig benchConfig() {
  SafetyConfig config;
  config.requireRadar = false;  // no interlock sensors in these tests
  config.requireLid = false;
  return config;
}

static void step(SafetyCore& core, uint32_t ms = 50) {
  now += ms;
  SafetyInputs in;
  in.currentUa = 0.0f;
  core.update(in, now);
}

// One command as the station sends it. params: the JSON object of the params.
static void command(char* out, size_t cap, const char* cmdId, const char* type, const char* params,
                    uint64_t ts = T0, int ttlS = 30, const char* version = "1.1") {
  snprintf(out, cap, "{\"schema_version\":\"%s\",\"cmd_id\":\"%s\",\"ts\":%llu,\"ttl_s\":%d,\"type\":\"%s\",\"params\":%s}",
           version, cmdId, static_cast<unsigned long long>(ts), ttlS, type, params);
}

// Runs one command through the handler. unixMs: the node's clock (synced unless told otherwise).
static aeroion_ack_t send(CommandHandler& handler, const char* json, uint64_t unixMs = T0 + 200, bool synced = true) {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, json));
  return handler.handle(doc.as<JsonObjectConst>(), unixMs, synced, now);
}

static void assertAck(const char* cmdId, aeroion_ack_t result) {
  const NodeEvent* ack = sink.lastOf(AEROION_EVENT_CMD_ACK);
  TEST_ASSERT_NOT_NULL(ack);
  TEST_ASSERT_EQUAL_STRING(cmdId, ack->id);
  TEST_ASSERT_EQUAL_STRING(aeroion_ack_name(result), aeroion_ack_name(ack->ack));
  TEST_ASSERT_TRUE(strlen(ack->text) <= 64);
  if (result == AEROION_ACK_ACCEPTED) {
    TEST_ASSERT_EQUAL_STRING("", ack->text);  // the contract: empty when accepted
  } else {
    TEST_ASSERT_TRUE(strlen(ack->text) > 0);  // a refusal always says why
  }
}

// ---------------------------------------------------------------- lines

static char* feedText(LineReader& reader, const char* text) {
  char* line = nullptr;
  for (const char* p = text; *p; ++p) {
    char* done = reader.feed(static_cast<unsigned char>(*p));
    if (done) line = done;
  }
  return line;
}

void test_lines_end_with_cr_lf_or_both_and_empty_lines_are_skipped() {
  char buffer[32];
  LineReader reader(buffer, sizeof buffer);
  TEST_ASSERT_EQUAL_STRING("state", feedText(reader, "state\r"));
  TEST_ASSERT_NULL(feedText(reader, "\n"));  // the \n of \r\n is an empty line
  TEST_ASSERT_EQUAL_STRING("badges", feedText(reader, "badges\n"));
  TEST_ASSERT_EQUAL_STRING("op", feedText(reader, "\r\n\r\nop\r\n"));
}

void test_a_line_too_long_is_dropped_whole() {
  char buffer[8];  // 7 characters at most
  LineReader reader(buffer, sizeof buffer);
  TEST_ASSERT_EQUAL_STRING("1234567", feedText(reader, "1234567\n"));
  TEST_ASSERT_NULL(feedText(reader, "12345678\n"));
  TEST_ASSERT_NULL(feedText(reader, "{\"topic\":\"aeroion/n01/cmd\"}\n"));
  TEST_ASSERT_EQUAL_UINT32(2, reader.dropped());
  TEST_ASSERT_EQUAL_STRING("ok", feedText(reader, "ok\n"));  // the next line is fine again
}

void test_backspace_and_noise() {
  char buffer[16];
  LineReader reader(buffer, sizeof buffer);
  TEST_ASSERT_EQUAL_STRING("tech", feedText(reader, "teck\x08h\n"));
  TEST_ASSERT_EQUAL_STRING("op", feedText(reader, "o\x01\x1bp\x7f\x7fop\n"));
  TEST_ASSERT_NULL(reader.feed(-1));
  TEST_ASSERT_NULL(reader.feed(0));
}

// ---------------------------------------------------------------- the clock

void test_the_clock_counts_from_the_build_time_until_the_link_sets_it() {
  UnixClock clock(T0 - 3600000ULL);  // built an hour ago
  TEST_ASSERT_TRUE(clock.now(0) == T0 - 3600000ULL);
  TEST_ASSERT_TRUE(clock.now(5000) == T0 - 3600000ULL + 5000);
  TEST_ASSERT_FALSE(clock.synced(5000));

  TEST_ASSERT_TRUE(clock.set(T0, 6000));
  TEST_ASSERT_TRUE(clock.now(6000) == T0);
  TEST_ASSERT_TRUE(clock.now(9500) == T0 + 3500);
  TEST_ASSERT_TRUE(clock.synced(9500));
  TEST_ASSERT_TRUE(clock.synced(6000 + 599999));
  TEST_ASSERT_FALSE(clock.synced(6000 + 600000));  // 10 minutes without a time line
  TEST_ASSERT_TRUE(clock.everSet());
}

void test_the_clock_refuses_times_that_cannot_be_real() {
  UnixClock clock(T0);
  TEST_ASSERT_FALSE(clock.set(0, 100));
  TEST_ASSERT_FALSE(clock.set(1791747040ULL, 100));  // seconds instead of milliseconds
  TEST_ASSERT_FALSE(clock.set(T0 * 1000ULL, 100));   // microseconds
  TEST_ASSERT_FALSE(clock.synced(100));
  TEST_ASSERT_TRUE(clock.now(100) == T0 + 100);
}

void test_a_time_read_a_little_earlier_is_not_a_wrap() {
  UnixClock clock(T0);
  TEST_ASSERT_TRUE(clock.now(5000) == T0 + 5000);
  TEST_ASSERT_TRUE(clock.now(4990) == T0 + 4990);  // main.cpp reads the time once per loop, events later
  TEST_ASSERT_TRUE(clock.now(5010) == T0 + 5010);
}

void test_the_clock_survives_the_49_day_wrap_of_the_millisecond_counter() {
  UnixClock clock(T0);
  TEST_ASSERT_TRUE(clock.set(T0, 0xFFFFF000UL));
  TEST_ASSERT_TRUE(clock.now(0xFFFFFFFFUL) == T0 + 0xFFF);
  TEST_ASSERT_TRUE(clock.now(0x00000100UL) == T0 + 0x1100);  // the counter wrapped
  TEST_ASSERT_TRUE(clock.synced(0x00000100UL));
}

// ---------------------------------------------------------------- commands

void test_lock_stops_an_armed_node() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  step(core);
  core.badge(kOperator, AEROION_ROLE_OPERATOR);
  TEST_ASSERT_EQUAL(AEROION_STATE_ARMED, core.state());
  char json[256];
  command(json, sizeof json, "c-lock-1", "LOCK", "{\"reason\":\"storm coming\"}");
  TEST_ASSERT_EQUAL(AEROION_ACK_ACCEPTED, send(handler, json));
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
  TEST_ASSERT_EQUAL_STRING("station LOCK: storm coming", core.lastReason());
  assertAck("c-lock-1", AEROION_ACK_ACCEPTED);
  TEST_ASSERT_TRUE(handler.ackSent());
  TEST_ASSERT_EQUAL_STRING("LOCK", handler.lastType());
}

void test_lock_is_refused_during_maintenance() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  step(core);
  core.badge(kTechnician, AEROION_ROLE_TECHNICIAN);
  TEST_ASSERT_EQUAL(AEROION_STATE_MAINTENANCE, core.state());
  char json[256];
  command(json, sizeof json, "c-lock-2", "LOCK", "{\"reason\":\"test\"}");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  TEST_ASSERT_EQUAL(AEROION_STATE_MAINTENANCE, core.state());
  assertAck("c-lock-2", AEROION_ACK_REJECTED);
}

void test_prearm_needs_armed_and_counts_eta_from_the_command_time() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  step(core);
  char json[256];
  command(json, sizeof json, "c-pre-1", "PREARM", "{\"eta_s\":10,\"source_node\":\"n02\"}");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));  // still LOCKED
  assertAck("c-pre-1", AEROION_ACK_REJECTED);

  core.badge(kOperator, AEROION_ROLE_OPERATOR);
  command(json, sizeof json, "c-pre-2", "PREARM", "{\"eta_s\":10,\"source_node\":\"n02\"}");
  TEST_ASSERT_EQUAL(AEROION_ACK_ACCEPTED, send(handler, json, T0 + 8000));  // it arrives 8 s after ts
  assertAck("c-pre-2", AEROION_ACK_ACCEPTED);
  // fog due 2 s from now, then 60 s more
  TEST_ASSERT_TRUE(core.prearmed(now + 61000));
  TEST_ASSERT_FALSE(core.prearmed(now + 63000));
}

void test_prearm_with_bad_params_is_refused() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  step(core);
  core.badge(kOperator, AEROION_ROLE_OPERATOR);
  char json[256];
  command(json, sizeof json, "c-pre-3", "PREARM", "{\"eta_s\":4000,\"source_node\":\"n02\"}");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  command(json, sizeof json, "c-pre-4", "PREARM", "{\"eta_s\":30}");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  TEST_ASSERT_FALSE(core.prearmed(now));
}

void test_set_hint_holds_until_the_command_expires() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  char json[256];
  TEST_ASSERT_EQUAL_FLOAT(0.6f, handler.requestedLevel(0.6f, now));
  command(json, sizeof json, "c-hint-1", "SET_HINT", "{\"hv_level\":0.45}", T0, 30);
  TEST_ASSERT_EQUAL(AEROION_ACK_ACCEPTED, send(handler, json, T0 + 10000));  // 10 s old: 20 s left
  assertAck("c-hint-1", AEROION_ACK_ACCEPTED);
  TEST_ASSERT_TRUE(handler.hintActive(now));
  TEST_ASSERT_EQUAL_FLOAT(0.45f, handler.requestedLevel(0.6f, now + 19999));
  TEST_ASSERT_EQUAL_FLOAT(0.6f, handler.requestedLevel(0.6f, now + 20000));
  TEST_ASSERT_FALSE(handler.hintActive(now + 20000));
}

void test_set_hint_without_a_synced_clock_holds_for_ttl() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  char json[256];
  command(json, sizeof json, "c-hint-2", "SET_HINT", "{\"hv_level\":1}", T0, 30);
  TEST_ASSERT_EQUAL(AEROION_ACK_ACCEPTED, send(handler, json, T0 - 7200000ULL, false));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, handler.requestedLevel(0.6f, now + 29999));
  TEST_ASSERT_EQUAL_FLOAT(0.6f, handler.requestedLevel(0.6f, now + 30000));
}

void test_set_hint_out_of_range_is_refused() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  char json[256];
  command(json, sizeof json, "c-hint-3", "SET_HINT", "{\"hv_level\":1.5}");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  command(json, sizeof json, "c-hint-4", "SET_HINT", "{\"level\":0.5}");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  TEST_ASSERT_FALSE(handler.hintActive(now));
  assertAck("c-hint-4", AEROION_ACK_REJECTED);
}

void test_sync_allowlist_replaces_the_badges_and_refuses_an_older_list() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  badges.set(kOperator, AEROION_ROLE_OPERATOR);
  CommandHandler handler(core, badges, sink);
  char json[512];
  command(json, sizeof json, "acl-5", "SYNC_ALLOWLIST",
          "{\"version\":5,\"entries\":[{\"uid\":\"04A1B2C3\",\"role\":\"operator\"},"
          "{\"uid\":\"04112233445566\",\"role\":\"technician\"}]}");
  TEST_ASSERT_EQUAL(AEROION_ACK_ACCEPTED, send(handler, json));
  assertAck("acl-5", AEROION_ACK_ACCEPTED);
  TEST_ASSERT_EQUAL(2, badges.count());
  TEST_ASSERT_EQUAL(AEROION_ROLE_TECHNICIAN, badges.roleOf("04112233445566"));
  TEST_ASSERT_EQUAL(AEROION_ROLE_UNKNOWN, badges.roleOf(kOperator));  // replaced, not merged
  TEST_ASSERT_EQUAL_UINT32(5, badges.version());

  command(json, sizeof json, "acl-4", "SYNC_ALLOWLIST", "{\"version\":4,\"entries\":[]}");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  assertAck("acl-4", AEROION_ACK_REJECTED);
  TEST_ASSERT_EQUAL(2, badges.count());
}

void test_tare_is_left_to_the_esp32() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  char json[256];
  command(json, sizeof json, "c-tare", "TARE", "{}");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  assertAck("c-tare", AEROION_ACK_REJECTED);
  TEST_ASSERT_NOT_NULL(strstr(sink.lastOf(AEROION_EVENT_CMD_ACK)->text, "ESP32"));
}

void test_an_old_command_expires_once_the_clock_is_synced() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  step(core);
  core.badge(kOperator, AEROION_ROLE_OPERATOR);
  char json[256];
  command(json, sizeof json, "c-old", "LOCK", "{\"reason\":\"late\"}", T0, 30);
  TEST_ASSERT_EQUAL(AEROION_ACK_EXPIRED, send(handler, json, T0 + 30000));
  assertAck("c-old", AEROION_ACK_EXPIRED);
  TEST_ASSERT_EQUAL(AEROION_STATE_ARMED, core.state());  // not run

  // Without a synced clock the node cannot tell the age: it runs the command.
  command(json, sizeof json, "c-old-2", "LOCK", "{\"reason\":\"late\"}", T0, 30);
  TEST_ASSERT_EQUAL(AEROION_ACK_ACCEPTED, send(handler, json, T0 + 3600000ULL, false));
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
}

void test_broken_commands_are_answered_with_the_reason() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  char json[256];
  command(json, sizeof json, "c-v2", "LOCK", "{\"reason\":\"x\"}", T0, 30, "2.0");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  TEST_ASSERT_NOT_NULL(strstr(sink.lastOf(AEROION_EVENT_CMD_ACK)->text, "schema_version 2.0"));
  command(json, sizeof json, "c-type", "REBOOT", "{}");
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  command(json, sizeof json, "c-ttl", "LOCK", "{\"reason\":\"x\"}", T0, 0);
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  command(json, sizeof json, "c-ttl-2", "LOCK", "{\"reason\":\"x\"}", T0, 601);
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED, send(handler, json));
  TEST_ASSERT_EQUAL(AEROION_ACK_REJECTED,
                    send(handler, "{\"schema_version\":\"1.1\",\"cmd_id\":\"c-np\",\"ts\":1,\"ttl_s\":5,\"type\":\"TARE\"}"));
  TEST_ASSERT_EQUAL_STRING("params missing", sink.lastOf(AEROION_EVENT_CMD_ACK)->text);
  TEST_ASSERT_EQUAL(5, sink.countOf(AEROION_EVENT_CMD_ACK));
  TEST_ASSERT_EQUAL_UINT32(5, handler.count(AEROION_ACK_REJECTED));
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
}

void test_a_command_without_cmd_id_cannot_be_answered() {
  SafetyCore core(benchConfig(), sink);
  BadgeList badges;
  CommandHandler handler(core, badges, sink);
  send(handler, "{\"schema_version\":\"1.1\",\"ts\":1,\"ttl_s\":5,\"type\":\"TARE\",\"params\":{}}");
  TEST_ASSERT_FALSE(handler.ackSent());
  send(handler, "{\"schema_version\":\"1.1\",\"cmd_id\":\"0123456789012345678901234567890123456\",\"ts\":1,"
                "\"ttl_s\":5,\"type\":\"TARE\",\"params\":{}}");  // 37 characters: too long
  TEST_ASSERT_FALSE(handler.ackSent());
  TEST_ASSERT_EQUAL(0, sink.count);
}

// ---------------------------------------------------------------- the link

struct LinkBench {
  SafetyCore core;
  BadgeList badges;
  UnixClock clock;
  CommandHandler handler;
  NodeLink link;
  LinkBench() : core(benchConfig(), sink), clock(T0 - 3600000ULL), handler(core, badges, sink),
                link("n01", clock, handler, sink) {}
};

void test_a_time_line_sets_the_clock_and_brings_the_link_up() {
  LinkBench b;
  TEST_ASSERT_FALSE(b.link.up());
  TEST_ASSERT_EQUAL(NodeLink::kTime, b.link.handleLine("{\"link\":\"time\",\"ts\":1791747040000}", 2000));
  TEST_ASSERT_TRUE(b.link.up());
  TEST_ASSERT_TRUE(b.clock.synced(2000));
  TEST_ASSERT_TRUE(b.clock.now(2500) == T0 + 500);
  TEST_ASSERT_TRUE(b.link.takeCameUp());
  TEST_ASSERT_FALSE(b.link.takeCameUp());  // once
  TEST_ASSERT_EQUAL(NodeLink::kUnknown, b.link.handleLine("{\"link\":\"time\",\"ts\":17917470}", 2100));
  TEST_ASSERT_TRUE(b.clock.now(2500) == T0 + 500);  // a time that cannot be real changes nothing
}

void test_commands_for_this_node_and_for_all_are_answered_others_ignored() {
  LinkBench b;
  char cmd[256], line[400];
  command(cmd, sizeof cmd, "c-1", "TARE", "{}");
  snprintf(line, sizeof line, "{\"topic\":\"aeroion/n01/cmd\",\"retain\":false,\"payload\":%s}", cmd);
  TEST_ASSERT_EQUAL(NodeLink::kCommand, b.link.handleLine(line, 1000));
  snprintf(line, sizeof line, "{\"topic\":\"aeroion/all/cmd\",\"payload\":%s}", cmd);
  TEST_ASSERT_EQUAL(NodeLink::kCommand, b.link.handleLine(line, 1000));
  snprintf(line, sizeof line, "{\"topic\":\"aeroion/n02/cmd\",\"retain\":false,\"payload\":%s}", cmd);
  TEST_ASSERT_EQUAL(NodeLink::kNotForUs, b.link.handleLine(line, 1000));
  TEST_ASSERT_EQUAL(2, sink.countOf(AEROION_EVENT_CMD_ACK));
  TEST_ASSERT_FALSE(b.link.up());  // commands alone do not make the link up: time lines do
}

void test_lines_that_are_not_for_the_link_are_counted_and_ignored() {
  LinkBench b;
  TEST_ASSERT_EQUAL(NodeLink::kBadJson, b.link.handleLine("{\"topic\":\"aeroion/n01/cmd\",", 1000));
  TEST_ASSERT_EQUAL_STRING("IncompleteInput", b.link.lastError());
  TEST_ASSERT_EQUAL(NodeLink::kBadJson, b.link.handleLine("[1,2,3]", 1000));
  TEST_ASSERT_EQUAL(NodeLink::kUnknown, b.link.handleLine("{\"hello\":1}", 1000));
  TEST_ASSERT_EQUAL(NodeLink::kUnknown, b.link.handleLine("{\"topic\":\"aeroion/n01/cmd\",\"payload\":7}", 1000));
  TEST_ASSERT_EQUAL_UINT32(2, b.link.lines(NodeLink::kBadJson));
  TEST_ASSERT_EQUAL_UINT32(2, b.link.lines(NodeLink::kUnknown));
  TEST_ASSERT_EQUAL(0, sink.count);
}

void test_comm_lost_once_when_the_link_goes_silent() {
  LinkBench b;
  b.link.poll(60000);
  TEST_ASSERT_EQUAL(0, sink.count);  // never up: nothing to lose
  b.link.handleLine("{\"link\":\"time\",\"ts\":1791747040000}", 1000);
  b.link.handleLine("{\"link\":\"time\",\"ts\":1791747045000}", 6000);
  b.link.poll(20999);
  TEST_ASSERT_EQUAL(0, sink.count);
  b.link.poll(21000);  // 15 s after the last time line
  b.link.poll(30000);
  TEST_ASSERT_EQUAL(1, sink.countOf(AEROION_EVENT_FAULT));
  TEST_ASSERT_EQUAL(AEROION_FAULT_COMM_LOST, sink.lastOf(AEROION_EVENT_FAULT)->fault);
  TEST_ASSERT_FALSE(b.link.up());

  b.link.takeCameUp();
  b.link.handleLine("{\"link\":\"time\",\"ts\":1791747080000}", 41000);
  TEST_ASSERT_TRUE(b.link.up());
  TEST_ASSERT_TRUE(b.link.takeCameUp());  // back: main.cpp sends info and status again
  b.link.poll(56000);
  TEST_ASSERT_EQUAL(2, sink.countOf(AEROION_EVENT_FAULT));
}

void test_a_full_badge_list_goes_through_the_link() {
  LinkBench b;
  static char line[4096];
  int n = snprintf(line, sizeof line,
                   "{\"topic\":\"aeroion/n01/cmd\",\"retain\":false,\"payload\":{\"schema_version\":\"1.1\","
                   "\"cmd_id\":\"acl-9-0123abcd\",\"ts\":1791747040000,\"ttl_s\":300,\"type\":\"SYNC_ALLOWLIST\","
                   "\"params\":{\"version\":9,\"entries\":[");
  for (int i = 0; i < BadgeList::kMaxBadges; ++i) {
    n += snprintf(line + n, sizeof line - n, "%s{\"uid\":\"04A1B2C3D4%04X\",\"role\":\"%s\"}", i ? "," : "", i,
                  i % 2 ? "household" : "operator");
  }
  snprintf(line + n, sizeof line - n, "]}}}");
  TEST_ASSERT_TRUE(strlen(line) < 4000);  // the MCX's line buffer is 4096 characters
  b.link.handleLine("{\"link\":\"time\",\"ts\":1791747040500}", 1000);
  TEST_ASSERT_EQUAL(NodeLink::kCommand, b.link.handleLine(line, 1100));
  assertAck("acl-9-0123abcd", AEROION_ACK_ACCEPTED);
  TEST_ASSERT_EQUAL(64, b.badges.count());
  TEST_ASSERT_EQUAL(AEROION_ROLE_HOUSEHOLD, b.badges.roleOf("04A1B2C3D4003F"));
  TEST_ASSERT_EQUAL_UINT32(9, b.badges.version());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lines_end_with_cr_lf_or_both_and_empty_lines_are_skipped);
  RUN_TEST(test_a_line_too_long_is_dropped_whole);
  RUN_TEST(test_backspace_and_noise);
  RUN_TEST(test_the_clock_counts_from_the_build_time_until_the_link_sets_it);
  RUN_TEST(test_the_clock_refuses_times_that_cannot_be_real);
  RUN_TEST(test_a_time_read_a_little_earlier_is_not_a_wrap);
  RUN_TEST(test_the_clock_survives_the_49_day_wrap_of_the_millisecond_counter);
  RUN_TEST(test_lock_stops_an_armed_node);
  RUN_TEST(test_lock_is_refused_during_maintenance);
  RUN_TEST(test_prearm_needs_armed_and_counts_eta_from_the_command_time);
  RUN_TEST(test_prearm_with_bad_params_is_refused);
  RUN_TEST(test_set_hint_holds_until_the_command_expires);
  RUN_TEST(test_set_hint_without_a_synced_clock_holds_for_ttl);
  RUN_TEST(test_set_hint_out_of_range_is_refused);
  RUN_TEST(test_sync_allowlist_replaces_the_badges_and_refuses_an_older_list);
  RUN_TEST(test_tare_is_left_to_the_esp32);
  RUN_TEST(test_an_old_command_expires_once_the_clock_is_synced);
  RUN_TEST(test_broken_commands_are_answered_with_the_reason);
  RUN_TEST(test_a_command_without_cmd_id_cannot_be_answered);
  RUN_TEST(test_a_time_line_sets_the_clock_and_brings_the_link_up);
  RUN_TEST(test_commands_for_this_node_and_for_all_are_answered_others_ignored);
  RUN_TEST(test_lines_that_are_not_for_the_link_are_counted_and_ignored);
  RUN_TEST(test_comm_lost_once_when_the_link_goes_silent);
  RUN_TEST(test_a_full_badge_list_goes_through_the_link);
  return UNITY_END();
}