#pragma once
#include <stdint.h>

#include "board_io.h"

// board_io.h for the NXP MCX A156 on the FRDM-MCXA156 board, with names from the board's Arduino header.
//
//   Analog inputs, all on ADC1 (16-bit, 0 V to the 3.3 V supply):
//     MCX_A0 = P1_10   MCX_A1 = P2_5   MCX_A2 = P2_3   MCX_A3 = P2_4   MCX_A4 = P1_12   MCX_A5 = P1_13
//   Digital pins: MCX_PIN(port, pin), e.g. MCX_PIN(3, 1) is P3_1, header pin D2.
//   I2C: LPI2C0 at 100 kHz on D14 (SDA, P0_16) and D15 (SCL, P0_17). The board's own temperature sensor
//   (P3T1755, address 0x48) is on the same two wires, so an I2C scan finds it with nothing plugged in.
//   SPI: driven by plain GPIO on D13 (SCK), D11 (MOSI) and D12 (MISO); chip select on any pin.

#ifdef __cplusplus
extern "C" {
#endif

enum { MCX_A0, MCX_A1, MCX_A2, MCX_A3, MCX_A4, MCX_A5 };

#define MCX_PIN(port, pin) ((uint8_t)((port) * 32U + (pin)))

// Header pins used so far (all the others: board manual UM12121)
#define MCX_D2 MCX_PIN(3, 1)
#define MCX_D4 MCX_PIN(3, 31)
#define MCX_D5 MCX_PIN(3, 14)
#define MCX_D8 MCX_PIN(1, 15)
#define MCX_D9 MCX_PIN(3, 17)
#define MCX_D11 MCX_PIN(3, 15)
#define MCX_D12 MCX_PIN(2, 16)
#define MCX_D13 MCX_PIN(2, 12)

// SPI pins of io_spi_transfer()
#define MCX_SPI_SCK MCX_D13
#define MCX_SPI_MOSI MCX_D11
#define MCX_SPI_MISO MCX_D12

// Sets up the ADC (with its calibration) and the I2C bus. Call once, after bench_board_init().
// adcFullScaleMv: the ADC reference, which is the board's 3.3 V supply.
void mcx_io_init(float adcFullScaleMv);

// A fast window on one analog input, for the arc predictor (M5a): n readings in millivolts, one every periodUs
// (paced on bench_micros(); one conversion each, no averaging). spanUs: the time from the first reading to the
// last, so the rate really achieved is (n - 1) / spanUs. Blocks for about n * periodUs (25.6 ms for 256 at
// 10 kHz). False if the input does not exist or the converter failed.
bool mcx_io_burst_mv(uint8_t input, float* mv, int n, uint32_t periodUs, uint32_t* spanUs);

#ifdef __cplusplus
}
#endif