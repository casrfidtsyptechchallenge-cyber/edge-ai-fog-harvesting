// Tests for the drivers' formulas (lib/sensing/sensor_math.h). They run on the PC: pio test -e native
#include <unity.h>

#include "sensor_math.h"

using namespace sensor_math;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- SHT45

void test_sht4x_crc_matches_the_datasheet_example() {
  const uint8_t data[2] = {0xBE, 0xEF};  // Sensirion's own example: CRC of 0xBEEF is 0x92
  TEST_ASSERT_EQUAL_HEX8(0x92, sht4xCrc8(data, 2));
}

void test_sht4x_temperature_range() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -45.0f, sht4xCelsius(0));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 130.0f, sht4xCelsius(65535));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.0f, sht4xCelsius(24716));  // (21 + 45) / 175 × 65535
}

void test_sht4x_humidity_is_clamped_to_0_100() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, sht4xPercentRh(0));        // formula gives -6 %
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, sht4xPercentRh(65535));  // formula gives 119 %
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 85.0f, sht4xPercentRh(47709));
}

// ---------------------------------------------------------------- HX711

void test_hx711_sign_extension() {
  TEST_ASSERT_EQUAL(0L, hx711Signed(0x000000UL));
  TEST_ASSERT_EQUAL(8388607L, hx711Signed(0x7FFFFFUL));   // largest positive
  TEST_ASSERT_EQUAL(-8388608L, hx711Signed(0x800000UL));  // largest negative
  TEST_ASSERT_EQUAL(-1L, hx711Signed(0xFFFFFFUL));
  TEST_ASSERT_EQUAL(50000L, hx711Signed(50000UL));
}

// ---------------------------------------------------------------- INA219

void test_ina219_shunt_voltage_is_signed() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, ina219ShuntMv(1000));     // 1000 × 10 µV
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -10.0f, ina219ShuntMv(0xFC18));  // -1000 in two's complement
}

void test_ina219_bus_voltage_ignores_the_status_bits() {
  uint16_t twelveVolts = static_cast<uint16_t>(3000 << 3);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, ina219BusVolts(twelveVolts));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, ina219BusVolts(twelveVolts | 0x2));  // "conversion ready" bit
  TEST_ASSERT_FALSE(ina219Overflow(twelveVolts | 0x2));
  TEST_ASSERT_TRUE(ina219Overflow(twelveVolts | 0x1));
}

// ---------------------------------------------------------------- optical fog

void test_fog_index_from_calibration() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, fogIndex(1800, 1800, 200));  // clear air
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, fogIndex(200, 1800, 200));   // densest fog
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, fogIndex(1000, 1800, 200));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, fogIndex(2000, 1800, 200));  // clamped
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, fogIndex(50, 1800, 200));    // clamped
}

// ---------------------------------------------------------------- collector current

void test_collector_current() {
  // 600 mV reading, 100 mV zero, gain 10, 1 kΩ shunt: (500 / 10) mV / 1000 Ω = 50 µA
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, collectorMicroamps(600, 100, 10, 1000));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, collectorMicroamps(100, 100, 10, 1000));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, collectorMicroamps(90, 100, 10, 1000));  // never negative
}

// ---------------------------------------------------------------- helpers

void test_smooth_moves_part_of_the_way() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, smooth(0.0f, 10.0f, 0.3f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, smooth(0.0f, 10.0f, 1.0f));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sht4x_crc_matches_the_datasheet_example);
  RUN_TEST(test_sht4x_temperature_range);
  RUN_TEST(test_sht4x_humidity_is_clamped_to_0_100);
  RUN_TEST(test_hx711_sign_extension);
  RUN_TEST(test_ina219_shunt_voltage_is_signed);
  RUN_TEST(test_ina219_bus_voltage_ignores_the_status_bits);
  RUN_TEST(test_fog_index_from_calibration);
  RUN_TEST(test_collector_current);
  RUN_TEST(test_smooth_moves_part_of_the_way);
  return UNITY_END();
}