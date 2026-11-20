// Tests for the safety core (lib/safety). They run on the PC: pio test -e native
#include <math.h>
#include <string.h>
#include <unity.h>

#include "fog_detector.h"
#include "safety_core.h"

static const char* kOperator = "0A0B0C0D";
static const char* kTechnician = "0E0E0E0E";

// Keeps every event so tests can check what was sent.
class RecordingSink : public EventSink {
 public:
  NodeEvent events[64];
  int count = 0;

  void emit(const NodeEvent& event) override {
    if (count < 64) events[count++] = event;
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

// Everything normal: nobody near, lid closed, no fog, no current.
static SafetyInputs calm() {
  SafetyInputs in;
  in.radarPresence = false;
  in.lidClosed = true;
  in.currentUa = 0.0f;
  return in;
}

static SafetyInputs foggy() {
  SafetyInputs in = calm();
  in.fogPresent = true;
  in.currentUa = 20.0f;
  return in;
}

static RecordingSink sink;
static uint32_t now;

void setUp() {
  sink.clear();
  now = 1000;
}
void tearDown() {}

static void step(SafetyCore& core, const SafetyInputs& in, uint32_t ms = 50) {
  now += ms;
  core.update(in, now);
  core.limitLevel(0.6f, now);
}

// A core already in HARVEST.
static void startHarvesting(SafetyCore& core) {
  step(core, calm());
  core.badge(kOperator, AEROION_ROLE_OPERATOR);
  step(core, foggy());
  sink.clear();
}

// ---------------------------------------------------------------- arming and disarming

void test_starts_locked_with_hv_off() {
  SafetyCore core(SafetyConfig(), sink);
  step(core, foggy());
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
  TEST_ASSERT_FALSE(core.hvAllowed());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, core.limitLevel(0.6f, now));
  TEST_ASSERT_EQUAL(0, sink.count);
}

void test_operator_badge_arms() {
  SafetyCore core(SafetyConfig(), sink);
  step(core, calm());
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL(AEROION_STATE_ARMED, core.state());
  TEST_ASSERT_FALSE(core.hvAllowed());  // armed = allowed, not on
  TEST_ASSERT_EQUAL(AEROION_EVENT_BADGE_TAP, sink.events[0].type);
  TEST_ASSERT_EQUAL(AEROION_BADGE_ARM, sink.events[0].action);
  TEST_ASSERT_EQUAL_STRING(kOperator, sink.events[0].id);
  TEST_ASSERT_EQUAL(AEROION_EVENT_STATE_CHANGE, sink.events[1].type);
  TEST_ASSERT_EQUAL_STRING("operator badge", sink.events[1].text);
}

void test_household_and_unknown_tags_are_denied() {
  SafetyCore core(SafetyConfig(), sink);
  step(core, calm());
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_DENIED, core.badge("11223344", AEROION_ROLE_HOUSEHOLD));
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_DENIED, core.badge("55667788", AEROION_ROLE_UNKNOWN));
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
  TEST_ASSERT_EQUAL(2, sink.countOf(AEROION_EVENT_BADGE_TAP));  // denied taps are logged too
}

void test_cannot_arm_with_lid_open() {
  SafetyCore core(SafetyConfig(), sink);
  SafetyInputs in = calm();
  in.lidClosed = false;
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_DENIED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
  TEST_ASSERT_EQUAL_STRING("lid open", core.blockingFault());
}

void test_cannot_arm_without_a_required_radar() {
  SafetyCore core(SafetyConfig(), sink);
  SafetyInputs in = calm();
  in.radarPresence.clear();
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_DENIED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL_STRING("radar not reporting", core.blockingFault());
}

void test_radar_can_be_made_optional() {
  SafetyConfig config;
  config.requireRadar = false;  // bench without a radar
  SafetyCore core(config, sink);
  SafetyInputs in = calm();
  in.radarPresence.clear();
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
}

void test_operator_badge_disarms_while_harvesting() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL(AEROION_BADGE_DISARM, sink.events[0].action);
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, core.limitLevel(0.6f, now));
}

// ---------------------------------------------------------------- fog

void test_fog_starts_and_ends_harvest() {
  SafetyCore core(SafetyConfig(), sink);
  step(core, calm());
  core.badge(kOperator, AEROION_ROLE_OPERATOR);
  step(core, foggy());
  TEST_ASSERT_EQUAL(AEROION_STATE_HARVEST, core.state());
  TEST_ASSERT_TRUE(core.hvAllowed());
  step(core, calm());
  TEST_ASSERT_EQUAL(AEROION_STATE_ARMED, core.state());
  TEST_ASSERT_EQUAL_STRING("fog cleared", core.lastReason());
}

// ---------------------------------------------------------------- people

void test_person_forces_safe_then_area_clear_rearms() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs in = foggy();
  in.radarPresence = true;
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_FALSE(core.hvAllowed());
  TEST_ASSERT_EQUAL(0, sink.countOf(AEROION_EVENT_FAULT));  // a person is not a fault
  step(core, foggy(), 9000);                                 // 9 s clear: not yet
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_FALSE(core.hvAllowed());
  step(core, foggy(), 1000);                                 // 10 s clear
  TEST_ASSERT_EQUAL(AEROION_STATE_ARMED, core.state());
  TEST_ASSERT_EQUAL_STRING("area clear", core.lastReason());
  step(core, foggy());
  TEST_ASSERT_EQUAL(AEROION_STATE_HARVEST, core.state());
}

// A presence radar can lose someone who stands still: one clear reading must not switch the HV back on.
void test_person_flickering_on_radar_keeps_the_node_safe() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs near = foggy();
  near.radarPresence = true;
  step(core, near);
  for (int i = 0; i < 20; ++i) {   // 20 s: seen, lost for 0.5 s, seen again...
    step(core, foggy(), 500);
    TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
    step(core, near, 500);
    TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  }
  TEST_ASSERT_EQUAL(0, sink.countOf(AEROION_EVENT_FAULT));
}

void test_presence_clear_time_is_configurable() {
  SafetyConfig config;
  config.presenceClearMs = 0;  // the old behaviour: re-arm at the first clear reading
  SafetyCore core(config, sink);
  startHarvesting(core);
  SafetyInputs near = foggy();
  near.radarPresence = true;
  step(core, near);
  step(core, foggy());
  TEST_ASSERT_EQUAL(AEROION_STATE_ARMED, core.state());
}

// ---------------------------------------------------------------- collector current sensor

void test_cannot_arm_without_the_collector_current() {
  SafetyCore core(SafetyConfig(), sink);
  SafetyInputs in = calm();
  in.currentUa.clear();
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_DENIED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL_STRING("collector current not reporting", core.blockingFault());
}

void test_losing_the_collector_current_while_harvesting_is_a_latched_fault() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs in = foggy();
  in.currentUa.clear();  // the current sensor stops: no arc detection any more
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_FALSE(core.hvAllowed());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, core.limitLevel(0.6f, now));
  TEST_ASSERT_EQUAL(AEROION_FAULT_SENSOR_FAIL, sink.lastOf(AEROION_EVENT_FAULT)->fault);
  step(core, foggy());  // it comes back: still SAFE until an operator resets
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL(AEROION_STATE_ARMED, core.state());
}

void test_collector_current_can_be_made_optional() {
  SafetyConfig config;
  config.requireCurrent = false;  // bench without the current sensor
  SafetyCore core(config, sink);
  SafetyInputs in = calm();
  in.currentUa.clear();
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
}

void test_operator_badge_while_person_near_disarms() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs in = foggy();
  in.radarPresence = true;  // the operator walks up...
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());  // ...and taps to stop it
  step(core, foggy());
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());  // stays stopped after they leave
}

// ---------------------------------------------------------------- faults

void test_lid_open_is_a_latched_fault() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs in = foggy();
  in.lidClosed = false;
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_EQUAL(AEROION_FAULT_LID_OPEN, sink.lastOf(AEROION_EVENT_FAULT)->fault);
  step(core, foggy());  // lid closed again
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());  // still needs a badge
}

void test_badge_resets_a_fault_only_when_its_cause_is_gone() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs open = foggy();
  open.lidClosed = false;
  step(core, open);
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_DENIED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  step(core, foggy());
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  TEST_ASSERT_EQUAL(AEROION_STATE_ARMED, core.state());
  TEST_ASSERT_EQUAL_STRING("reset by operator badge", core.lastReason());
}

void test_radar_failure_while_running_is_a_fault() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs in = foggy();
  in.radarPresence.clear();
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_EQUAL(AEROION_FAULT_SENSOR_FAIL, sink.lastOf(AEROION_EVENT_FAULT)->fault);
}

void test_lid_opening_during_a_presence_stop_becomes_a_fault() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs in = foggy();
  in.radarPresence = true;
  step(core, in);
  in.lidClosed = false;  // someone opens the box
  step(core, in);
  TEST_ASSERT_EQUAL(1, sink.countOf(AEROION_EVENT_FAULT));
  step(core, foggy());  // they leave and close it
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());  // no automatic restart
}

void test_lid_open_while_locked_only_blocks_arming() {
  SafetyCore core(SafetyConfig(), sink);
  SafetyInputs in = calm();
  in.lidClosed = false;
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
  TEST_ASSERT_EQUAL(0, sink.count);
}

// ---------------------------------------------------------------- maintenance

void test_technician_lockout_only_the_same_badge_releases_it() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kTechnician, AEROION_ROLE_TECHNICIAN));
  TEST_ASSERT_EQUAL(AEROION_STATE_MAINTENANCE, core.state());
  TEST_ASSERT_EQUAL(1, sink.countOf(AEROION_EVENT_MAINT_ON));
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_DENIED, core.badge("0F0F0F0F", AEROION_ROLE_TECHNICIAN));
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_DENIED, core.badge(kOperator, AEROION_ROLE_OPERATOR));
  SafetyInputs in = foggy();
  in.lidClosed = false;  // the technician opens the box: no fault in MAINTENANCE
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_STATE_MAINTENANCE, core.state());
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kTechnician, AEROION_ROLE_TECHNICIAN));
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
  TEST_ASSERT_EQUAL(1, sink.countOf(AEROION_EVENT_MAINT_OFF));
}

void test_technician_can_lock_out_a_node_in_safe() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs in = foggy();
  in.radarPresence = true;  // the radar sees the technician walking up
  step(core, in);
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_EQUAL(AEROION_BADGE_RESULT_GRANTED, core.badge(kTechnician, AEROION_ROLE_TECHNICIAN));
  TEST_ASSERT_EQUAL(AEROION_STATE_MAINTENANCE, core.state());
}

// ---------------------------------------------------------------- station commands

void test_station_lock_stops_everything() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyReply r = core.lock("storm");
  TEST_ASSERT_TRUE(r.accepted);
  TEST_ASSERT_EQUAL_STRING("", r.reason);
  TEST_ASSERT_EQUAL(AEROION_STATE_LOCKED, core.state());
  TEST_ASSERT_EQUAL_STRING("station LOCK: storm", core.lastReason());
}

void test_station_lock_cannot_end_a_lockout() {
  SafetyCore core(SafetyConfig(), sink);
  step(core, calm());
  core.badge(kTechnician, AEROION_ROLE_TECHNICIAN);
  SafetyReply r = core.lock("test");
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_EQUAL(AEROION_STATE_MAINTENANCE, core.state());
}

void test_prearm_only_when_armed_and_it_expires() {
  SafetyCore core(SafetyConfig(), sink);
  step(core, calm());
  SafetyReply r = core.prearm(30, now);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_EQUAL_STRING("state is LOCKED, PREARM needs ARMED", r.reason);
  core.badge(kOperator, AEROION_ROLE_OPERATOR);
  TEST_ASSERT_TRUE(core.prearm(30, now).accepted);
  TEST_ASSERT_TRUE(core.prearmed(now + 89000));   // eta 30 s + 60 s
  TEST_ASSERT_FALSE(core.prearmed(now + 90000));
}

// ---------------------------------------------------------------- arcs

void test_arc_prediction_backs_off_then_harvest_resumes() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  for (int i = 0; i < 100; ++i) step(core, foggy());  // ramp up for 5 s
  float before = core.limitLevel(0.6f, now);
  SafetyInputs risky = foggy();
  risky.arcRisk = 0.9f;
  step(core, risky);
  TEST_ASSERT_EQUAL(AEROION_STATE_BACKOFF, core.state());
  TEST_ASSERT_EQUAL(1, sink.countOf(AEROION_EVENT_ARC_PREDICTED));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, before - 0.12f, core.limitLevel(0.6f, now));
  for (int i = 0; i < 100; ++i) step(core, risky);  // risk stays high: no way out
  TEST_ASSERT_EQUAL(AEROION_STATE_BACKOFF, core.state());
  TEST_ASSERT_EQUAL(1, sink.countOf(AEROION_EVENT_ARC_PREDICTED));  // warned once, not on every update
  step(core, foggy());
  TEST_ASSERT_EQUAL(AEROION_STATE_HARVEST, core.state());
  TEST_ASSERT_EQUAL_STRING("backoff over", core.lastReason());
}

void test_current_spike_is_an_arc() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs spike = foggy();
  spike.currentUa = 400.0f;
  step(core, spike);
  step(core, spike);  // the same arc seen twice: counted once
  TEST_ASSERT_EQUAL(1, sink.countOf(AEROION_EVENT_ARC));
  TEST_ASSERT_EQUAL_FLOAT(400.0f, sink.lastOf(AEROION_EVENT_ARC)->peakUa);
  TEST_ASSERT_FALSE(sink.lastOf(AEROION_EVENT_ARC)->predicted);
  TEST_ASSERT_EQUAL(AEROION_STATE_BACKOFF, core.state());
}

void test_arc_soon_after_a_warning_counts_as_predicted() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs risky = foggy();
  risky.arcRisk = 0.9f;
  step(core, risky);
  risky.currentUa = 400.0f;
  step(core, risky, 1000);
  TEST_ASSERT_TRUE(sink.lastOf(AEROION_EVENT_ARC)->predicted);
}

void test_three_arcs_in_a_minute_is_a_fault() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs spike = foggy();
  spike.currentUa = 400.0f;
  for (int i = 0; i < 3; ++i) {
    step(core, spike);
    step(core, foggy(), 10000);
  }
  TEST_ASSERT_EQUAL(3, sink.countOf(AEROION_EVENT_ARC));
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_EQUAL(AEROION_FAULT_HV_OVERCURRENT, sink.lastOf(AEROION_EVENT_FAULT)->fault);
}

void test_arcs_far_apart_are_not_a_fault() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs spike = foggy();
  spike.currentUa = 400.0f;
  for (int i = 0; i < 3; ++i) {
    step(core, spike);
    step(core, foggy(), 40000);
  }
  TEST_ASSERT_EQUAL(3, sink.countOf(AEROION_EVENT_ARC));
  TEST_ASSERT_EQUAL(0, sink.countOf(AEROION_EVENT_FAULT));
}

void test_sustained_overcurrent_is_a_fault() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  SafetyInputs high = foggy();
  high.currentUa = 300.0f;
  for (int i = 0; i < 25; ++i) step(core, high);  // 1.25 s
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_EQUAL(AEROION_FAULT_HV_OVERCURRENT, sink.lastOf(AEROION_EVENT_FAULT)->fault);
}

void test_external_arc_report_is_ignored_when_hv_is_off() {
  SafetyCore core(SafetyConfig(), sink);
  step(core, calm());
  core.reportArc(500.0f, now);
  TEST_ASSERT_EQUAL(0, sink.countOf(AEROION_EVENT_ARC));
}

// ---------------------------------------------------------------- HV level

void test_level_starts_at_minimum_and_ramps_up_slowly() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  float first = core.limitLevel(0.9f, now + 50);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.30f, first);
  float later = core.limitLevel(0.9f, now + 2050);  // 2 s later: +0.10
  TEST_ASSERT_FLOAT_WITHIN(0.001f, first + 0.10f, later);
}

void test_level_never_exceeds_the_ceiling_and_drops_at_once() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  TEST_ASSERT_EQUAL_FLOAT(0.95f, core.limitLevel(1.5f, now + 60000));
  TEST_ASSERT_EQUAL_FLOAT(0.40f, core.limitLevel(0.40f, now + 60050));
}

void test_nan_request_gives_the_minimum() {
  SafetyCore core(SafetyConfig(), sink);
  startHarvesting(core);
  TEST_ASSERT_EQUAL_FLOAT(0.30f, core.limitLevel(NAN, now + 50));
}

// ---------------------------------------------------------------- fog detector

void test_fog_detector_uses_hysteresis() {
  FogDetector fog(SafetyConfig(), sink);
  fog.update(0.25f, AEROION_FOG_SOURCE_OPTICAL, false, 1000);
  TEST_ASSERT_FALSE(fog.present());
  fog.update(0.35f, AEROION_FOG_SOURCE_OPTICAL, false, 2000);
  TEST_ASSERT_TRUE(fog.present());
  fog.update(0.20f, AEROION_FOG_SOURCE_OPTICAL, false, 3000);  // between the thresholds: still fog
  TEST_ASSERT_TRUE(fog.present());
  fog.update(0.10f, AEROION_FOG_SOURCE_OPTICAL, false, 32000);
  TEST_ASSERT_FALSE(fog.present());
  TEST_ASSERT_EQUAL(1, sink.countOf(AEROION_EVENT_FOG_DETECTED));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.35f, sink.lastOf(AEROION_EVENT_FOG_DETECTED)->fogIdx);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, sink.lastOf(AEROION_EVENT_FOG_CLEARED)->durationS);
}

void test_fog_detector_lower_threshold_when_prearmed() {
  FogDetector fog(SafetyConfig(), sink);
  fog.update(0.25f, AEROION_FOG_SOURCE_OPTICAL, true, 1000);
  TEST_ASSERT_TRUE(fog.present());
}

void test_fog_detector_no_reading_means_no_fog() {
  FogDetector fog(SafetyConfig(), sink);
  fog.update(0.50f, AEROION_FOG_SOURCE_OPTICAL, false, 1000);
  fog.update(Nullable<float>(), AEROION_FOG_SOURCE_OPTICAL, false, 2000);
  TEST_ASSERT_FALSE(fog.present());
  TEST_ASSERT_EQUAL(1, sink.countOf(AEROION_EVENT_FOG_CLEARED));
}

void test_contract_uid_check() {
  TEST_ASSERT_TRUE(isContractUid("04A1B2C3"));
  TEST_ASSERT_TRUE(isContractUid("04A1B2C3D4E5F60718AB"));  // 20 characters
  TEST_ASSERT_FALSE(isContractUid("04a1b2c3"));  // lower case
  TEST_ASSERT_FALSE(isContractUid("04A1B2"));    // too short
  TEST_ASSERT_FALSE(isContractUid("04A1B2C3G"));
  TEST_ASSERT_FALSE(isContractUid(nullptr));
}

// A sensor whose wire came off still reports: about 0 µA. With the HV really on, that cannot be.
static SafetyConfig withHvOutput() {
  SafetyConfig c;
  c.minCurrentUa = 0.3f;
  return c;
}

static void climbTo(SafetyCore& core, float level, SafetyInputs in) {
  for (int i = 0; i < 40; ++i) {  // 2 s at 0.05/s from 0.30: past 0.45
    now += 50;
    core.update(in, now);
    core.limitLevel(level, now);
  }
}

void test_a_disconnected_current_sensor_is_a_fault_once_the_hv_is_up() {
  SafetyCore core(withHvOutput(), sink);
  startHarvesting(core);
  SafetyInputs dead = foggy();
  dead.currentUa = 0.05f;
  climbTo(core, 0.6f, dead);
  TEST_ASSERT_EQUAL(AEROION_STATE_HARVEST, core.state());  // not yet: 3 s to come up
  for (int i = 0; i < 70 && core.state() == AEROION_STATE_HARVEST; ++i) climbTo(core, 0.6f, dead);
  TEST_ASSERT_EQUAL(AEROION_STATE_SAFE, core.state());
  TEST_ASSERT_EQUAL(AEROION_FAULT_SENSOR_FAIL, sink.lastOf(AEROION_EVENT_FAULT)->fault);
  TEST_ASSERT_FALSE(core.hvAllowed());
}

void test_little_current_near_the_onset_is_not_a_fault() {
  SafetyCore core(withHvOutput(), sink);
  startHarvesting(core);
  SafetyInputs low = foggy();
  low.currentUa = 0.05f;
  for (int i = 0; i < 400; ++i) {  // 20 s at the minimum level 0.30, below minCurrentFromLevel
    now += 50;
    core.update(low, now);
    core.limitLevel(0.30f, now);
  }
  TEST_ASSERT_EQUAL(AEROION_STATE_HARVEST, core.state());
}

void test_a_short_dip_of_current_is_not_a_fault() {
  SafetyCore core(withHvOutput(), sink);
  startHarvesting(core);
  climbTo(core, 0.6f, foggy());
  SafetyInputs dip = foggy();
  dip.currentUa = 0.0f;
  for (int k = 0; k < 10; ++k) {  // 2 s low, 1 s normal, again and again
    for (int i = 0; i < 40; ++i) { now += 50; core.update(dip, now); core.limitLevel(0.6f, now); }
    for (int i = 0; i < 20; ++i) { now += 50; core.update(foggy(), now); core.limitLevel(0.6f, now); }
  }
  TEST_ASSERT_EQUAL(AEROION_STATE_HARVEST, core.state());
}

void test_the_plausibility_check_is_off_without_an_hv_output() {
  SafetyCore core(SafetyConfig(), sink);  // default: minCurrentUa 0, as on the bench today
  startHarvesting(core);
  SafetyInputs dead = foggy();
  dead.currentUa = 0.0f;
  for (int i = 0; i < 20; ++i) climbTo(core, 0.6f, dead);
  TEST_ASSERT_EQUAL(AEROION_STATE_HARVEST, core.state());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_locked_with_hv_off);
  RUN_TEST(test_operator_badge_arms);
  RUN_TEST(test_household_and_unknown_tags_are_denied);
  RUN_TEST(test_cannot_arm_with_lid_open);
  RUN_TEST(test_cannot_arm_without_a_required_radar);
  RUN_TEST(test_radar_can_be_made_optional);
  RUN_TEST(test_operator_badge_disarms_while_harvesting);
  RUN_TEST(test_fog_starts_and_ends_harvest);
  RUN_TEST(test_person_forces_safe_then_area_clear_rearms);
  RUN_TEST(test_person_flickering_on_radar_keeps_the_node_safe);
  RUN_TEST(test_presence_clear_time_is_configurable);
  RUN_TEST(test_cannot_arm_without_the_collector_current);
  RUN_TEST(test_losing_the_collector_current_while_harvesting_is_a_latched_fault);
  RUN_TEST(test_collector_current_can_be_made_optional);
  RUN_TEST(test_a_disconnected_current_sensor_is_a_fault_once_the_hv_is_up);
  RUN_TEST(test_little_current_near_the_onset_is_not_a_fault);
  RUN_TEST(test_a_short_dip_of_current_is_not_a_fault);
  RUN_TEST(test_the_plausibility_check_is_off_without_an_hv_output);
  RUN_TEST(test_operator_badge_while_person_near_disarms);
  RUN_TEST(test_lid_open_is_a_latched_fault);
  RUN_TEST(test_badge_resets_a_fault_only_when_its_cause_is_gone);
  RUN_TEST(test_radar_failure_while_running_is_a_fault);
  RUN_TEST(test_lid_opening_during_a_presence_stop_becomes_a_fault);
  RUN_TEST(test_lid_open_while_locked_only_blocks_arming);
  RUN_TEST(test_technician_lockout_only_the_same_badge_releases_it);
  RUN_TEST(test_technician_can_lock_out_a_node_in_safe);
  RUN_TEST(test_station_lock_stops_everything);
  RUN_TEST(test_station_lock_cannot_end_a_lockout);
  RUN_TEST(test_prearm_only_when_armed_and_it_expires);
  RUN_TEST(test_arc_prediction_backs_off_then_harvest_resumes);
  RUN_TEST(test_current_spike_is_an_arc);
  RUN_TEST(test_arc_soon_after_a_warning_counts_as_predicted);
  RUN_TEST(test_three_arcs_in_a_minute_is_a_fault);
  RUN_TEST(test_arcs_far_apart_are_not_a_fault);
  RUN_TEST(test_sustained_overcurrent_is_a_fault);
  RUN_TEST(test_external_arc_report_is_ignored_when_hv_is_off);
  RUN_TEST(test_level_starts_at_minimum_and_ramps_up_slowly);
  RUN_TEST(test_level_never_exceeds_the_ceiling_and_drops_at_once);
  RUN_TEST(test_nan_request_gives_the_minimum);
  RUN_TEST(test_fog_detector_uses_hysteresis);
  RUN_TEST(test_fog_detector_lower_threshold_when_prearmed);
  RUN_TEST(test_fog_detector_no_reading_means_no_fog);
  RUN_TEST(test_contract_uid_check);
  return UNITY_END();
}