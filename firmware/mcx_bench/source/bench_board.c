#include "bench_board.h"

#include <string.h>

#include "app.h"  // BOARD_InitHardware(), from hello_world's hardware_init.c
#include "board.h"
#include "fsl_clock.h"
#include "fsl_gpio.h"
#include "fsl_lpuart.h"
#include "fsl_port.h"
#include "fsl_reset.h"
#include "fsl_wwdt.h"

static volatile uint32_t s_millis;

void SysTick_Handler(void) { s_millis++; }

// RGB LED (P3_12 red, P3_13 green, P3_0 blue, lit when LOW) and buttons SW2 (P1_7), SW3 (P0_6), LOW when
// pressed. Same settings as the SDK's gpio examples for this board.
static void init_led_and_buttons(void) {
  CLOCK_EnableClock(kCLOCK_GateGPIO0);
  CLOCK_EnableClock(kCLOCK_GateGPIO1);
  CLOCK_EnableClock(kCLOCK_GateGPIO3);
  CLOCK_EnableClock(kCLOCK_GatePORT0);
  CLOCK_EnableClock(kCLOCK_GatePORT1);
  CLOCK_EnableClock(kCLOCK_GatePORT3);
  RESET_ReleasePeripheralReset(kGPIO0_RST_SHIFT_RSTn);
  RESET_ReleasePeripheralReset(kGPIO1_RST_SHIFT_RSTn);
  RESET_ReleasePeripheralReset(kGPIO3_RST_SHIFT_RSTn);
  RESET_ReleasePeripheralReset(kPORT1_RST_SHIFT_RSTn);
  RESET_ReleasePeripheralReset(kPORT3_RST_SHIFT_RSTn);

  const port_pin_config_t output = {kPORT_PullDisable,      kPORT_LowPullResistor,  kPORT_FastSlewRate,
                                    kPORT_PassiveFilterDisable, kPORT_OpenDrainDisable, kPORT_LowDriveStrength,
                                    kPORT_NormalDriveStrength,  kPORT_MuxAlt0,          kPORT_InputBufferEnable,
                                    kPORT_InputNormal,      kPORT_UnlockRegister};
  const port_pin_config_t button = {kPORT_PullUp,           kPORT_LowPullResistor,  kPORT_FastSlewRate,
                                    kPORT_PassiveFilterDisable, kPORT_OpenDrainDisable, kPORT_LowDriveStrength,
                                    kPORT_NormalDriveStrength,  kPORT_MuxAlt0,          kPORT_InputBufferEnable,
                                    kPORT_InputNormal,      kPORT_UnlockRegister};
  PORT_SetPinConfig(PORT3, BOARD_LED_RED_GPIO_PIN, &output);
  PORT_SetPinConfig(PORT3, BOARD_LED_GREEN_GPIO_PIN, &output);
  PORT_SetPinConfig(PORT3, BOARD_LED_BLUE_GPIO_PIN, &output);
  PORT_SetPinConfig(PORT1, BOARD_SW2_GPIO_PIN, &button);
  PORT_SetPinConfig(PORT0, BOARD_SW3_GPIO_PIN, &button);

  const gpio_pin_config_t ledOff = {kGPIO_DigitalOutput, 1U};
  const gpio_pin_config_t input = {kGPIO_DigitalInput, 0U};
  GPIO_PinInit(BOARD_LED_RED_GPIO, BOARD_LED_RED_GPIO_PIN, &ledOff);
  GPIO_PinInit(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_GPIO_PIN, &ledOff);
  GPIO_PinInit(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_GPIO_PIN, &ledOff);
  GPIO_PinInit(BOARD_SW2_GPIO, BOARD_SW2_GPIO_PIN, &input);
  GPIO_PinInit(BOARD_SW3_GPIO, BOARD_SW3_GPIO_PIN, &input);
}

void bench_board_init(void) {
  BOARD_InitHardware();  // UART pins, 96 MHz clock, debug console: exactly as hello_world
  init_led_and_buttons();
  SysTick_Config(SystemCoreClock / 1000U);
}

uint32_t bench_millis(void) { return s_millis; }

// The 1 ms tick plus how far SysTick has counted down since. Read again if the tick came in between.
uint32_t bench_micros(void) {
  uint32_t ms, ticks;
  do {
    ms = s_millis;
    ticks = SysTick->VAL;
  } while (ms != s_millis);
  uint32_t load = SysTick->LOAD + 1U;
  return ms * 1000U + (uint32_t)(((uint64_t)(load - 1U - ticks) * 1000U) / load);
}

void bench_led(bool red, bool green, bool blue) {
  GPIO_PinWrite(BOARD_LED_RED_GPIO, BOARD_LED_RED_GPIO_PIN, red ? 0U : 1U);
  GPIO_PinWrite(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_GPIO_PIN, green ? 0U : 1U);
  GPIO_PinWrite(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_GPIO_PIN, blue ? 0U : 1U);
}

bool bench_sw2_down(void) { return GPIO_PinRead(BOARD_SW2_GPIO, BOARD_SW2_GPIO_PIN) == 0U; }

bool bench_sw3_down(void) { return GPIO_PinRead(BOARD_SW3_GPIO, BOARD_SW3_GPIO_PIN) == 0U; }

void bench_write(const char* text) { LPUART_WriteBlocking(LPUART0, (const uint8_t*)text, strlen(text)); }

int bench_read(void) {
  uint32_t flags = LPUART_GetStatusFlags(LPUART0);
  if (flags & kLPUART_RxOverrunFlag) {
    LPUART_ClearStatusFlags(LPUART0, kLPUART_RxOverrunFlag);  // otherwise reception stops
  }
  return (flags & kLPUART_RxDataRegFullFlag) ? LPUART_ReadByte(LPUART0) : -1;
}

bool bench_watchdog_begin(uint32_t timeoutMs) {
  CLOCK_SetClockDiv(kCLOCK_DivWWDT0, 1U);  // the watchdog runs from the 1 MHz clock
  uint32_t clockHz = CLOCK_GetWwdtClkFreq();
  if (clockHz == 0U) return false;
  wwdt_config_t config;
  WWDT_GetDefaultConfig(&config);
  config.timeoutValue = clockHz / 4U / 1000U * timeoutMs;  // the counter runs at a quarter of its clock
  config.windowValue = 0xFFFFFFU;                           // no window: it may be fed at any time
  config.warningValue = 0U;
  config.enableWatchdogReset = true;
  config.clockFreq_Hz = clockHz;
  WWDT_Init(WWDT0, &config);
  return true;
}

void bench_watchdog_feed(void) { WWDT_Refresh(WWDT0); }

bench_restart_t bench_restart_cause(void) {
  uint32_t sources = CMC->SRS;  // system reset status: why the chip last restarted
  if (sources & CMC_SRS_WWDT0_MASK) return BENCH_RESTART_WATCHDOG;
  if (sources & CMC_SRS_LOCKUP_MASK) return BENCH_RESTART_CRASH;
  if (sources & CMC_SRS_VD_MASK) return BENCH_RESTART_BROWNOUT;
  return BENCH_RESTART_NORMAL;
}