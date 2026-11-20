#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The hardware operations the portable sensor drivers in this folder need. Each board implements them once:
//   firmware/mcx_node/source/mcx_io.c    NXP MCX A156 (FRDM-MCXA156 board)
//   test/test_drivers/test_main.cpp      a fake board, for the tests on the PC
// Pins and analog inputs are plain numbers; the board's own file says which pin each number is.
// Plain C, so a board can implement it with the chip maker's C SDK.

#ifdef __cplusplus
extern "C" {
#endif

// One analog reading in millivolts. False if the input does not exist or the converter failed.
bool io_analog_mv(uint8_t input, float* mv);

// Digital pins.
void io_pin_input_pullup(uint8_t pin);       // input with a pull-up: an unconnected pin reads HIGH
bool io_pin_read(uint8_t pin);               // true = HIGH
void io_pin_output(uint8_t pin, bool high);  // output, starting at this level
void io_pin_write(uint8_t pin, bool high);

void io_delay_us(uint32_t us);

// I2C with 7-bit addresses. False if the device does not answer or the bus is stuck.
bool io_i2c_write(uint8_t address, const uint8_t* data, size_t length);  // length 0: only checks it answers
bool io_i2c_write_read(uint8_t address, const uint8_t* out, size_t outLength, uint8_t* in, size_t inLength);

// SPI mode 0, most significant bit first. csPin is held LOW for the whole transfer. `in` may be NULL.
// False if the board has no SPI.
bool io_spi_transfer(uint8_t csPin, const uint8_t* out, uint8_t* in, size_t length);

#ifdef __cplusplus
}
#endif