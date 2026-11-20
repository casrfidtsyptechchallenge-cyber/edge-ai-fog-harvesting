#pragma once
#include <stdint.h>

// The two serial ports of the node's MCX A156, both run by interrupts with a ring buffer each way, so the
// safety loop never waits for a slow line and no received byte is lost while the loop is busy.
//
//   MCX_UART_USB   LPUART0, the MCU-Link USB serial port to the PC (115200 baud, set by bench_board_init()).
//   MCX_UART_LINK  LPUART2 on the Arduino header, to the node's ESP32-S3 (M7):
//                    D1 = P2_10 = TX  -> ESP32 RX
//                    D0 = P2_11 = RX  <- ESP32 TX       and GND to GND. 3.3 V logic on both chips.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MCX_UART_USB = 0, MCX_UART_LINK = 1 } mcx_uart_t;

// Takes LPUART0 over from bench_board_init() and sets up LPUART2 at linkBaud. Call once, right after
// bench_board_init(); from then on use these functions, not bench_write() / bench_read().
void mcx_uart_init(uint32_t linkBaud);

// Queues the text for sending. Returns at once, unless the queue is full: then it waits for room.
void mcx_uart_write(mcx_uart_t port, const char* text);

// Next received byte (0 to 255), or -1 if none is waiting.
int mcx_uart_read(mcx_uart_t port);

// Received bytes lost since boot: the receive queue was full, or the hardware overran.
uint32_t mcx_uart_lost(mcx_uart_t port);

#ifdef __cplusplus
}
#endif