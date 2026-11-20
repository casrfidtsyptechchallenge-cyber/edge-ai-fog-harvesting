#pragma once
#include <stdbool.h>
#include <stdint.h>

// Board support for the M3 test bench on the NXP FRDM-MCXA156 (MCUXpresso SDK).
// Everything that touches the SDK is here, in C; the rest of the bench is portable C++.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  BENCH_RESTART_NORMAL,    // power-up, reset button, debugger
  BENCH_RESTART_WATCHDOG,  // the watchdog was not fed in time
  BENCH_RESTART_CRASH,     // the CPU locked up (fault inside a fault)
  BENCH_RESTART_BROWNOUT,  // the supply voltage dropped
} bench_restart_t;

void bench_board_init(void);   // clocks, USB serial port, RGB LED, buttons, 1 ms tick
uint32_t bench_millis(void);   // milliseconds since boot
uint32_t bench_micros(void);   // microseconds since boot (wraps after 71 minutes): timing the arc predictor
void bench_led(bool red, bool green, bool blue);
bool bench_sw2_down(void);     // true while SW2 is pressed
bool bench_sw3_down(void);     // true while SW3 is pressed
void bench_write(const char* text);  // to the PC, through the MCU-Link USB serial port
int bench_read(void);          // next character from the PC, or -1 if none
bool bench_watchdog_begin(uint32_t timeoutMs);  // false if the watchdog clock is missing
void bench_watchdog_feed(void);
bench_restart_t bench_restart_cause(void);

#ifdef __cplusplus
}
#endif