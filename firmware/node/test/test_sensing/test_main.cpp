// Tests for the sensing core (lib/sensing). They run on the PC: pio test -e native
#include <unity.h>

#include "nullable.h"
#include "rate_estimator.h"
#include "sensor_registry.h"

// A sensor whose behaviour each test controls.
class TestSensor : public Sensor {
 public:
  bool beginResult = true;
  PollResult pollResult = PollResult::Ok;
  float value = 0;
  int begins = 0;
  int polls = 0;
  uint32_t period = 100;

  const char* name() const override { return "TEST"; }
  bool begin() override {
    begins++;
    return beginResult;
  }
  uint32_t periodMs() const override { return period; }
  PollResult poll(uint32_t) override {
    polls++;
    return pollResult;
  }
  void fill(SensorFrame& frame) const override { frame.tempC = value; }
};

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- Nullable

void test_nullable_starts_empty() {
  Nullable<float> v;
  TEST_ASSERT_FALSE(v.has());
  TEST_ASSERT_EQUAL_FLOAT(7.0f, v.getOr(7.0f));
}

void test_nullable_holds_and_clears_a_value() {
  Nullable<float> v;
  v = 21.5f;
  TEST_ASSERT_TRUE(v.has());
  TEST_ASSERT_EQUAL_FLOAT(21.5f, v.get());
  v.clear();
  TEST_ASSERT_FALSE(v.has());
}

void test_frame_clear_empties_every_field() {
  SensorFrame frame;
  frame.tempC = 20.0f;
  frame.lidClosed = true;
  frame.clear();
  TEST_ASSERT_FALSE(frame.tempC.has());
  TEST_ASSERT_FALSE(frame.lidClosed.has());
}

// ---------------------------------------------------------------- RateEstimator

void test_rate_estimator_needs_samples() {
  RateEstimator<30> rate;
  TEST_ASSERT_FALSE(rate.ready());
  rate.add(0, 1.0f);
  TEST_ASSERT_FALSE(rate.ready());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, rate.perMinute());
}

void test_rate_estimator_finds_the_slope() {
  RateEstimator<30> rate;
  for (int s = 0; s < 20; ++s) rate.add(s * 1000, 0.5f * s / 60.0f);  // 0.5 g per minute
  TEST_ASSERT_TRUE(rate.ready());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, rate.perMinute());
}

void test_rate_estimator_keeps_only_the_last_n_samples() {
  RateEstimator<10> rate;
  for (int s = 0; s < 10; ++s) rate.add(s * 1000, 0.0f);           // flat at first
  for (int s = 10; s < 20; ++s) rate.add(s * 1000, (s - 10) / 60.0f);  // then 1 g/min
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, rate.perMinute());
}

void test_rate_estimator_survives_millis_wrapping() {
  RateEstimator<30> rate;
  uint32_t start = 0xFFFFFFFFu - 5000;  // millis() wraps to 0 after about 49 days
  for (int s = 0; s < 10; ++s) rate.add(start + s * 1000, 2.0f * s / 60.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, rate.perMinute());
}

// ---------------------------------------------------------------- SensorRegistry

void test_registry_refuses_null_and_overflow() {
  SensorRegistry registry;
  TestSensor sensors[SensorRegistry::kMaxSensors + 1];
  TEST_ASSERT_FALSE(registry.add(nullptr));
  for (int i = 0; i < SensorRegistry::kMaxSensors; ++i) TEST_ASSERT_TRUE(registry.add(&sensors[i]));
  TEST_ASSERT_FALSE(registry.add(&sensors[SensorRegistry::kMaxSensors]));
  TEST_ASSERT_EQUAL(SensorRegistry::kMaxSensors, registry.count());
}

void test_registry_polls_each_sensor_at_its_own_period() {
  SensorRegistry registry;
  TestSensor fast, slow;
  fast.period = 100;
  slow.period = 1000;
  registry.add(&fast);
  registry.add(&slow);
  registry.beginAll(0);
  for (uint32_t t = 0; t <= 2000; t += 10) registry.pollAll(t);
  TEST_ASSERT_EQUAL(20, fast.polls);
  TEST_ASSERT_EQUAL(2, slow.polls);
}

void test_registry_fills_only_fresh_sensors() {
  SensorRegistry registry;
  TestSensor sensor;
  sensor.value = 21.5f;
  registry.add(&sensor);
  registry.beginAll(0);
  registry.pollAll(100);

  SensorFrame frame;
  registry.fillFrame(frame, 100);
  TEST_ASSERT_TRUE(frame.tempC.has());
  TEST_ASSERT_EQUAL_FLOAT(21.5f, frame.tempC.get());
  TEST_ASSERT_EQUAL(static_cast<int>(SensorState::Ok), static_cast<int>(registry.stateAt(0, 100)));

  // The sensor starts failing: after staleAfterMs its value must disappear (null), not stay frozen.
  sensor.pollResult = PollResult::Error;
  uint32_t later = 100 + sensor.staleAfterMs() + 1;
  for (uint32_t t = 200; t <= later; t += 100) registry.pollAll(t);
  registry.fillFrame(frame, later);
  TEST_ASSERT_FALSE(frame.tempC.has());
  TEST_ASSERT_EQUAL(static_cast<int>(SensorState::Stale), static_cast<int>(registry.stateAt(0, later)));
  TEST_ASSERT_TRUE(registry.errorsAt(0) > 0);
}

void test_registry_retries_a_missing_sensor() {
  SensorRegistry registry;
  TestSensor sensor;
  sensor.beginResult = false;
  registry.add(&sensor);
  registry.beginAll(0);
  TEST_ASSERT_EQUAL(static_cast<int>(SensorState::Missing), static_cast<int>(registry.stateAt(0, 0)));

  registry.pollAll(SensorRegistry::kRetryMs - 1);
  TEST_ASSERT_EQUAL(1, sensor.begins);  // not retried too early

  sensor.beginResult = true;  // someone plugs it in
  registry.pollAll(SensorRegistry::kRetryMs);
  TEST_ASSERT_EQUAL(2, sensor.begins);
  registry.pollAll(SensorRegistry::kRetryMs + 100);
  TEST_ASSERT_EQUAL(static_cast<int>(SensorState::Ok),
                    static_cast<int>(registry.stateAt(0, SensorRegistry::kRetryMs + 100)));
}

void test_registry_restarts_a_sensor_after_many_errors() {
  SensorRegistry registry;
  TestSensor sensor;
  sensor.pollResult = PollResult::Error;
  registry.add(&sensor);
  registry.beginAll(0);
  uint32_t t = 0;
  for (uint32_t i = 0; i < SensorRegistry::kMaxConsecutiveErrors; ++i) registry.pollAll(t += 100);
  TEST_ASSERT_EQUAL(1, sensor.begins);
  registry.pollAll(t + SensorRegistry::kRetryMs);
  TEST_ASSERT_EQUAL(2, sensor.begins);
}

void test_not_ready_is_not_an_error() {
  SensorRegistry registry;
  TestSensor sensor;
  sensor.pollResult = PollResult::NotReady;
  registry.add(&sensor);
  registry.beginAll(0);
  for (uint32_t t = 100; t <= 3000; t += 100) registry.pollAll(t);
  TEST_ASSERT_EQUAL(0, static_cast<int>(registry.errorsAt(0)));
  TEST_ASSERT_EQUAL(static_cast<int>(SensorState::Waiting), static_cast<int>(registry.stateAt(0, 3000)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_nullable_starts_empty);
  RUN_TEST(test_nullable_holds_and_clears_a_value);
  RUN_TEST(test_frame_clear_empties_every_field);
  RUN_TEST(test_rate_estimator_needs_samples);
  RUN_TEST(test_rate_estimator_finds_the_slope);
  RUN_TEST(test_rate_estimator_keeps_only_the_last_n_samples);
  RUN_TEST(test_rate_estimator_survives_millis_wrapping);
  RUN_TEST(test_registry_refuses_null_and_overflow);
  RUN_TEST(test_registry_polls_each_sensor_at_its_own_period);
  RUN_TEST(test_registry_fills_only_fresh_sensors);
  RUN_TEST(test_registry_retries_a_missing_sensor);
  RUN_TEST(test_registry_restarts_a_sensor_after_many_errors);
  RUN_TEST(test_not_ready_is_not_an_error);
  return UNITY_END();
}