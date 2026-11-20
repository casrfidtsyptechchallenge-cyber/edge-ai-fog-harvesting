// Tests for the portable sensor drivers (lib/drivers), on a fake board. They run on the PC:
// pio test -e native
#include <string.h>
#include <unity.h>

#include "board_io.h"
#include "collector_current_driver.h"
#include "ina219_driver.h"
#include "optical_fog_driver.h"
#include "sensor_registry.h"
#include "switch_drivers.h"

// ---------------------------------------------------------------- the fake board

namespace fake {
const int kInputs = 8;
const int kPins = 64;

float analogMv[kInputs];  // what each analog input reads
bool analogBroken;        // every analog reading fails
int litInput;             // this input reads ledExtraMv more while ledPin is HIGH
int ledPin;
float ledExtraMv;
bool level[kPins];  // what an input reads, or what an output was set to
bool pullup[kPins];
bool output[kPins];
uint32_t delayedUs;
bool inaPresent;      // an INA219 answers at 0x40
uint16_t inaRegs[6];  // its registers

void reset() {
  memset(analogMv, 0, sizeof analogMv);
  analogBroken = false;
  litInput = -1;
  ledPin = -1;
  ledExtraMv = 0;
  memset(level, 0, sizeof level);
  memset(pullup, 0, sizeof pullup);
  memset(output, 0, sizeof output);
  delayedUs = 0;
  inaPresent = false;
  memset(inaRegs, 0, sizeof inaRegs);
}
}  // namespace fake

extern "C" {
bool io_analog_mv(uint8_t input, float* mv) {
  if (fake::analogBroken || input >= fake::kInputs) return false;
  *mv = fake::analogMv[input];
  if (input == fake::litInput && fake::ledPin >= 0 && fake::level[fake::ledPin]) *mv += fake::ledExtraMv;
  return true;
}
void io_pin_input_pullup(uint8_t pin) {
  fake::pullup[pin] = true;
  fake::output[pin] = false;
}
bool io_pin_read(uint8_t pin) { return fake::level[pin]; }
void io_pin_output(uint8_t pin, bool high) {
  fake::output[pin] = true;
  fake::level[pin] = high;
}
void io_pin_write(uint8_t pin, bool high) { fake::level[pin] = high; }
void io_delay_us(uint32_t us) { fake::delayedUs += us; }
bool io_i2c_write(uint8_t address, const uint8_t* data, size_t length) {
  if (address != 0x40 || !fake::inaPresent) return false;
  if (length == 3 && data[0] < 6) fake::inaRegs[data[0]] = static_cast<uint16_t>((data[1] << 8) | data[2]);
  return true;
}
bool io_i2c_write_read(uint8_t address, const uint8_t* out, size_t outLength, uint8_t* in, size_t inLength) {
  if (address != 0x40 || !fake::inaPresent || outLength != 1 || inLength != 2 || out[0] >= 6) return false;
  in[0] = static_cast<uint8_t>(fake::inaRegs[out[0]] >> 8);
  in[1] = static_cast<uint8_t>(fake::inaRegs[out[0]] & 0xFF);
  return true;
}
}

void setUp() { fake::reset(); }
void tearDown() {}

// ---------------------------------------------------------------- collector current

void test_collector_subtracts_the_zero_measured_at_begin() {
  CollectorCurrentDriver driver(0, 1000.0f, 10.0f);  // 1 kΩ shunt, gain 10: 10 mV per µA
  fake::analogMv[0] = 500.0f;
  TEST_ASSERT_TRUE(driver.begin());
  TEST_ASSERT_EQUAL_FLOAT(500.0f, driver.zeroMv());

  fake::analogMv[0] = 600.0f;
  TEST_ASSERT_TRUE(driver.poll(0) == PollResult::Ok);
  SensorFrame frame;
  driver.fill(frame);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, frame.currentUa.get());
}

void test_collector_current_is_never_negative() {
  CollectorCurrentDriver driver(0, 1000.0f, 10.0f);
  fake::analogMv[0] = 500.0f;
  driver.begin();
  fake::analogMv[0] = 400.0f;
  driver.poll(0);
  SensorFrame frame;
  driver.fill(frame);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, frame.currentUa.get());
}

// The safety core must see a spike in the very next reading: no smoothing across readings.
void test_collector_shows_a_spike_at_once() {
  CollectorCurrentDriver driver(0, 1000.0f, 10.0f);
  driver.begin();
  driver.poll(0);  // 0 µA
  fake::analogMv[0] = 3000.0f;
  driver.poll(100);  // 300 µA, above the 250 µA arc threshold
  SensorFrame frame;
  driver.fill(frame);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 300.0f, frame.currentUa.get());
}

// Set up again while the HV is on, the driver must not take the corona current as its new zero.
void test_collector_keeps_its_zero_when_set_up_again() {
  CollectorCurrentDriver driver(0, 1000.0f, 10.0f);
  fake::analogMv[0] = 500.0f;
  driver.begin();
  fake::analogMv[0] = 900.0f;  // HV on: 40 µA
  TEST_ASSERT_TRUE(driver.begin());
  TEST_ASSERT_EQUAL_FLOAT(500.0f, driver.zeroMv());
  driver.poll(0);
  SensorFrame frame;
  driver.fill(frame);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, frame.currentUa.get());

  TEST_ASSERT_TRUE(driver.zero());  // on request only
  TEST_ASSERT_EQUAL_FLOAT(900.0f, driver.zeroMv());
}

void test_collector_reports_converter_errors() {
  CollectorCurrentDriver driver(0, 1000.0f, 10.0f);
  fake::analogBroken = true;
  TEST_ASSERT_FALSE(driver.begin());  // no zero: missing
  fake::analogBroken = false;
  TEST_ASSERT_TRUE(driver.begin());
  fake::analogBroken = true;
  TEST_ASSERT_TRUE(driver.poll(0) == PollResult::Error);
}

void test_collector_refuses_a_wrong_calibration() {
  CollectorCurrentDriver noShunt(0, 0.0f, 10.0f);
  CollectorCurrentDriver noGain(0, 1000.0f, 0.0f);
  TEST_ASSERT_FALSE(noShunt.begin());
  TEST_ASSERT_FALSE(noGain.begin());
}

// ---------------------------------------------------------------- optical fog sensor and fog knob

void test_optical_subtracts_ambient_light() {
  OpticalFogDriver driver(5, 1, 1800.0f, 200.0f);
  fake::ledPin = 5;
  fake::litInput = 1;
  fake::analogMv[1] = 700.0f;    // room light
  fake::ledExtraMv = 1000.0f;    // the LED adds 1000 mV through the fog
  TEST_ASSERT_TRUE(driver.begin());
  TEST_ASSERT_TRUE(fake::output[5]);
  TEST_ASSERT_TRUE(driver.poll(0) == PollResult::Ok);
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, driver.lastSignalMv());
  SensorFrame frame;
  driver.fill(frame);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, frame.fogIdxOpt.get());  // (1800 - 1000) / (1800 - 200)
  TEST_ASSERT_FALSE(fake::level[5]);                             // LED off again
  TEST_ASSERT_TRUE(fake::delayedUs > 0);                         // waited for the LED to light
}

void test_optical_rejects_a_signal_out_of_range() {
  OpticalFogDriver driver(5, 1, 1800.0f, 200.0f);
  fake::ledPin = 5;
  fake::litInput = 1;
  driver.begin();
  fake::ledExtraMv = 50.0f;  // below half the dense signal: unplugged or blocked
  TEST_ASSERT_TRUE(driver.poll(0) == PollResult::Error);
  fake::ledExtraMv = 3000.0f;  // above 1.5 × the clear signal
  TEST_ASSERT_TRUE(driver.poll(200) == PollResult::Error);
}

void test_optical_refuses_a_wrong_calibration() {
  OpticalFogDriver driver(5, 1, 200.0f, 1800.0f);  // clear and dense swapped
  TEST_ASSERT_FALSE(driver.begin());
}

void test_fog_knob_maps_the_voltage_to_a_fog_index() {
  FogKnobDriver driver(2, 3300.0f);
  TEST_ASSERT_TRUE(driver.begin());
  fake::analogMv[2] = 1650.0f;
  TEST_ASSERT_TRUE(driver.poll(0) == PollResult::Ok);
  SensorFrame frame;
  driver.fill(frame);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, frame.fogIdxOpt.get());

  fake::analogMv[2] = 4000.0f;
  driver.poll(200);
  driver.fill(frame);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, frame.fogIdxOpt.get());

  fake::analogBroken = true;
  TEST_ASSERT_TRUE(driver.poll(400) == PollResult::Error);
}

// ---------------------------------------------------------------- INA219

void test_ina219_is_missing_when_nothing_answers() {
  Ina219Driver driver(0.1f);
  TEST_ASSERT_FALSE(driver.begin());
}

void test_ina219_writes_its_configuration_and_reads_the_power() {
  Ina219Driver driver(0.1f);
  fake::inaPresent = true;
  TEST_ASSERT_TRUE(driver.begin());
  TEST_ASSERT_EQUAL_HEX16(0x399F, fake::inaRegs[0]);

  fake::inaRegs[1] = 1000;               // 10 mV across 0.1 Ω: 0.1 A
  fake::inaRegs[2] = (12000 / 4) << 3;   // 12.000 V
  TEST_ASSERT_TRUE(driver.poll(0) == PollResult::Ok);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, driver.busVolts());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, driver.currentMa());
  SensorFrame frame;
  driver.fill(frame);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.2f, frame.powerW.get());
}

void test_ina219_adds_up_energy_and_skips_long_gaps() {
  Ina219Driver driver(0.1f);
  fake::inaPresent = true;
  driver.begin();
  fake::inaRegs[1] = 1000;
  fake::inaRegs[2] = (12000 / 4) << 3;  // 1.2 W
  driver.poll(0);
  driver.poll(1000);  // 1.2 W for 1 s
  SensorFrame frame;
  driver.fill(frame);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.2f / 3600.0f, frame.energyWh.get());

  driver.poll(10000);  // 9 s without readings: not counted
  driver.fill(frame);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.2f / 3600.0f, frame.energyWh.get());
}

void test_ina219_reports_an_overflow() {
  Ina219Driver driver(0.1f);
  fake::inaPresent = true;
  driver.begin();
  fake::inaRegs[2] = 0x0001;  // overflow bit
  TEST_ASSERT_TRUE(driver.poll(0) == PollResult::Error);
}

// ---------------------------------------------------------------- radar and lid switch

void test_radar_uses_a_pullup_and_high_means_present() {
  RadarPinDriver driver(7);
  TEST_ASSERT_TRUE(driver.begin());
  TEST_ASSERT_TRUE(fake::pullup[7]);
  SensorFrame frame;

  fake::level[7] = true;  // someone in range, or a broken wire
  driver.poll(0);
  driver.fill(frame);
  TEST_ASSERT_TRUE(frame.radarPresence.get());

  fake::level[7] = false;
  driver.poll(100);
  driver.fill(frame);
  TEST_ASSERT_FALSE(frame.radarPresence.get());
}

void test_lid_needs_two_equal_readings() {
  LidPinDriver driver(9);
  TEST_ASSERT_TRUE(driver.begin());
  TEST_ASSERT_TRUE(fake::pullup[9]);
  SensorFrame frame;

  fake::level[9] = false;  // switch closed: lid closed
  TEST_ASSERT_TRUE(driver.poll(0) == PollResult::NotReady);
  TEST_ASSERT_TRUE(driver.poll(50) == PollResult::Ok);
  driver.fill(frame);
  TEST_ASSERT_TRUE(frame.lidClosed.get());

  fake::level[9] = true;  // one bounce: ignored
  driver.poll(100);
  fake::level[9] = false;
  driver.poll(150);
  driver.fill(frame);
  TEST_ASSERT_TRUE(frame.lidClosed.get());

  fake::level[9] = true;  // really open
  driver.poll(200);
  driver.poll(250);
  driver.fill(frame);
  TEST_ASSERT_FALSE(frame.lidClosed.get());
}

// ---------------------------------------------------------------- with the registry

void test_a_missing_sensor_leaves_only_its_own_fields_null() {
  CollectorCurrentDriver current(0, 1000.0f, 10.0f);
  Ina219Driver power(0.1f);  // nothing answers at 0x40
  LidPinDriver lid(9);
  SensorRegistry registry;
  registry.add(&current);
  registry.add(&power);
  registry.add(&lid);

  registry.beginAll(0);
  for (uint32_t t = 0; t <= 300; t += 50) registry.pollAll(t);
  SensorFrame frame;
  registry.fillFrame(frame, 300);

  TEST_ASSERT_TRUE(frame.currentUa.has());
  TEST_ASSERT_TRUE(frame.lidClosed.has());
  TEST_ASSERT_FALSE(frame.powerW.has());
  TEST_ASSERT_FALSE(frame.energyWh.has());
  TEST_ASSERT_TRUE(registry.stateAt(1, 300) == SensorState::Missing);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_collector_subtracts_the_zero_measured_at_begin);
  RUN_TEST(test_collector_current_is_never_negative);
  RUN_TEST(test_collector_shows_a_spike_at_once);
  RUN_TEST(test_collector_keeps_its_zero_when_set_up_again);
  RUN_TEST(test_collector_reports_converter_errors);
  RUN_TEST(test_collector_refuses_a_wrong_calibration);
  RUN_TEST(test_optical_subtracts_ambient_light);
  RUN_TEST(test_optical_rejects_a_signal_out_of_range);
  RUN_TEST(test_optical_refuses_a_wrong_calibration);
  RUN_TEST(test_fog_knob_maps_the_voltage_to_a_fog_index);
  RUN_TEST(test_ina219_is_missing_when_nothing_answers);
  RUN_TEST(test_ina219_writes_its_configuration_and_reads_the_power);
  RUN_TEST(test_ina219_adds_up_energy_and_skips_long_gaps);
  RUN_TEST(test_ina219_reports_an_overflow);
  RUN_TEST(test_radar_uses_a_pullup_and_high_means_present);
  RUN_TEST(test_lid_needs_two_equal_readings);
  RUN_TEST(test_a_missing_sensor_leaves_only_its_own_fields_null);
  return UNITY_END();
}