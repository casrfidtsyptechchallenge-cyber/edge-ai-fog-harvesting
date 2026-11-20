// mcx_uart.h on the NXP MCX A156 (MCUXpresso SDK). One interrupt per port moves bytes between the LPUART and
// two ring buffers; the main loop only touches the buffers. Clock and pin settings as the SDK's board files
// set up LPUART0 for the debug console.
#include "mcx_uart.h"

#include <stddef.h>

#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_lpuart.h"
#include "fsl_port.h"
#include "fsl_reset.h"

#define RX_SIZE 4096U  // a whole SYNC_ALLOWLIST line fits, even while the main loop is busy
#define TX_SIZE 2048U  // both sizes must be powers of two
#define RX_ERRORS (LPUART_STAT_OR_MASK | LPUART_STAT_NF_MASK | LPUART_STAT_FE_MASK | LPUART_STAT_PF_MASK)

// head and tail count bytes forever (they wrap at 2^32); the index in the buffer is count % size.
typedef struct {
  LPUART_Type* base;
  IRQn_Type irq;
  volatile uint8_t rx[RX_SIZE];
  volatile uint32_t rxHead;  // written only by the interrupt
  volatile uint32_t rxTail;  // written only by the main loop
  volatile uint8_t tx[TX_SIZE];
  volatile uint32_t txHead;  // written only by the main loop
  volatile uint32_t txTail;  // written only by the interrupt
  volatile uint32_t lost;
} uart_port_t;

static uart_port_t s_ports[2];

static void service(uart_port_t* u) {
  LPUART_Type* base = u->base;
  if (base == NULL) return;

  while (base->STAT & LPUART_STAT_RDRF_MASK) {
    uint8_t byte = (uint8_t)base->DATA;
    uint32_t head = u->rxHead;
    if (head - u->rxTail < RX_SIZE) {
      u->rx[head & (RX_SIZE - 1U)] = byte;
      u->rxHead = head + 1U;
    } else {
      u->lost++;  // the main loop has not read for a long time
    }
  }
  uint32_t errors = base->STAT & RX_ERRORS;
  if (errors) {
    if (errors & LPUART_STAT_OR_MASK) u->lost++;  // the receiver stops until this flag is cleared
    (void)LPUART_ClearStatusFlags(base, kLPUART_RxOverrunFlag | kLPUART_NoiseErrorFlag |
                                            kLPUART_FramingErrorFlag | kLPUART_ParityErrorFlag);
  }

  if (base->CTRL & LPUART_CTRL_TIE_MASK) {
    while ((base->STAT & LPUART_STAT_TDRE_MASK) && u->txTail != u->txHead) {
      base->DATA = u->tx[u->txTail & (TX_SIZE - 1U)];
      u->txTail = u->txTail + 1U;
    }
    if (u->txTail == u->txHead) base->CTRL &= ~LPUART_CTRL_TIE_MASK;  // all sent: stop asking
  }
}

void LPUART0_IRQHandler(void);  // replaces the SDK's handler from the startup file
void LPUART0_IRQHandler(void) {
  service(&s_ports[MCX_UART_USB]);
  SDK_ISR_EXIT_BARRIER;
}

void LPUART2_IRQHandler(void);
void LPUART2_IRQHandler(void) {
  service(&s_ports[MCX_UART_LINK]);
  SDK_ISR_EXIT_BARRIER;
}

static void start(uart_port_t* u, LPUART_Type* base, IRQn_Type irq) {
  u->base = base;
  u->irq = irq;
  (void)LPUART_ClearStatusFlags(base, kLPUART_RxOverrunFlag | kLPUART_NoiseErrorFlag | kLPUART_FramingErrorFlag |
                                          kLPUART_ParityErrorFlag);
  base->CTRL |= LPUART_CTRL_RIE_MASK | LPUART_CTRL_ORIE_MASK;  // an interrupt for every received byte
  EnableIRQ(irq);
}

// LPUART2 on D0 (P2_11, RX) and D1 (P2_10, TX), function ALT3 of these pins, 12 MHz clock.
static bool init_link(uint32_t baud) {
  CLOCK_AttachClk(kFRO12M_to_LPUART2);
  CLOCK_SetClockDiv(kCLOCK_DivLPUART2, 1U);
  RESET_PeripheralReset(kLPUART2_RST_SHIFT_RSTn);
  CLOCK_EnableClock(kCLOCK_GatePORT2);
  RESET_ReleasePeripheralReset(kPORT2_RST_SHIFT_RSTn);

  // The pull-up keeps RX idle (HIGH) while no ESP32 is connected, so the node reads no noise.
  const port_pin_config_t pin = {kPORT_PullUp,           kPORT_LowPullResistor,  kPORT_FastSlewRate,
                                 kPORT_PassiveFilterDisable, kPORT_OpenDrainDisable, kPORT_LowDriveStrength,
                                 kPORT_NormalDriveStrength,  kPORT_MuxAlt3,          kPORT_InputBufferEnable,
                                 kPORT_InputNormal,      kPORT_UnlockRegister};
  PORT_SetPinConfig(PORT2, 10U, &pin);  // D1: LPUART2_TXD
  PORT_SetPinConfig(PORT2, 11U, &pin);  // D0: LPUART2_RXD

  lpuart_config_t config;
  LPUART_GetDefaultConfig(&config);
  config.baudRate_Bps = baud;
  config.enableTx = true;
  config.enableRx = true;
  return LPUART_Init(LPUART2, &config, CLOCK_GetLpuartClkFreq(2U)) == kStatus_Success;
}

void mcx_uart_init(uint32_t linkBaud) {
  start(&s_ports[MCX_UART_USB], LPUART0, LPUART0_IRQn);  // already set up by BOARD_InitDebugConsole()
  if (init_link(linkBaud)) start(&s_ports[MCX_UART_LINK], LPUART2, LPUART2_IRQn);
}

void mcx_uart_write(mcx_uart_t port, const char* text) {
  uart_port_t* u = &s_ports[port];
  if (u->base == NULL || text == NULL) return;
  while (*text != '\0') {
    uint32_t head = u->txHead;
    if (head - u->txTail < TX_SIZE) {
      u->tx[head & (TX_SIZE - 1U)] = (uint8_t)*text++;
      u->txHead = head + 1U;
      continue;
    }
    // Full: make sure the interrupt is sending, and wait for room.
    DisableIRQ(u->irq);
    u->base->CTRL |= LPUART_CTRL_TIE_MASK;
    EnableIRQ(u->irq);
  }
  DisableIRQ(u->irq);  // the interrupt also changes CTRL: no change of it in between
  u->base->CTRL |= LPUART_CTRL_TIE_MASK;
  EnableIRQ(u->irq);
}

int mcx_uart_read(mcx_uart_t port) {
  uart_port_t* u = &s_ports[port];
  uint32_t tail = u->rxTail;
  if (tail == u->rxHead) return -1;
  int byte = u->rx[tail & (RX_SIZE - 1U)];
  u->rxTail = tail + 1U;
  return byte;
}

uint32_t mcx_uart_lost(mcx_uart_t port) { return s_ports[port].lost; }