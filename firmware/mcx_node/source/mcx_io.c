// board_io.h on the NXP MCX A156, with the MCUXpresso SDK drivers. Settings follow the SDK's lpadc and lpi2c
// examples for the FRDM-MCXA156.
#include "mcx_io.h"

#include <string.h>

#include "bench_board.h"  // bench_micros(), to pace the fast windows
#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_gpio.h"
#include "fsl_lpadc.h"
#include "fsl_lpi2c.h"
#include "fsl_port.h"
#include "fsl_reset.h"

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

// ---------------------------------------------------------------- pins

static PORT_Type* const s_ports[] = PORT_BASE_PTRS;
static GPIO_Type* const s_gpios[] = GPIO_BASE_PTRS;
static const clock_ip_name_t s_portClocks[] = {kCLOCK_GatePORT0, kCLOCK_GatePORT1, kCLOCK_GatePORT2,
                                               kCLOCK_GatePORT3, kCLOCK_GatePORT4};
static const clock_ip_name_t s_gpioClocks[] = {kCLOCK_GateGPIO0, kCLOCK_GateGPIO1, kCLOCK_GateGPIO2,
                                               kCLOCK_GateGPIO3, kCLOCK_GateGPIO4};
static const reset_ip_name_t s_portResets[] = {kPORT0_RST_SHIFT_RSTn, kPORT1_RST_SHIFT_RSTn, kPORT2_RST_SHIFT_RSTn,
                                               kPORT3_RST_SHIFT_RSTn, kPORT4_RST_SHIFT_RSTn};
static const reset_ip_name_t s_gpioResets[] = {kGPIO0_RST_SHIFT_RSTn, kGPIO1_RST_SHIFT_RSTn, kGPIO2_RST_SHIFT_RSTn,
                                               kGPIO3_RST_SHIFT_RSTn, kGPIO4_RST_SHIFT_RSTn};

// Turns on the pin's port and GPIO, then sets its function (mux), pull-up and digital input buffer.
static bool setup_pin(uint8_t pin, port_mux_t mux, bool pullUp, bool digital) {
  uint32_t port = pin / 32U;
  if (port >= COUNT_OF(s_ports)) return false;
  CLOCK_EnableClock(s_portClocks[port]);
  CLOCK_EnableClock(s_gpioClocks[port]);
  RESET_ReleasePeripheralReset(s_portResets[port]);
  RESET_ReleasePeripheralReset(s_gpioResets[port]);
  const port_pin_config_t config = {pullUp ? kPORT_PullUp : kPORT_PullDisable,
                                    kPORT_LowPullResistor,
                                    kPORT_FastSlewRate,
                                    kPORT_PassiveFilterDisable,
                                    kPORT_OpenDrainDisable,
                                    kPORT_LowDriveStrength,
                                    kPORT_NormalDriveStrength,
                                    mux,
                                    digital ? kPORT_InputBufferEnable : kPORT_InputBufferDisable,
                                    kPORT_InputNormal,
                                    kPORT_UnlockRegister};
  PORT_SetPinConfig(s_ports[port], pin % 32U, &config);
  return true;
}

void io_pin_input_pullup(uint8_t pin) {
  if (!setup_pin(pin, kPORT_MuxAlt0, true, true)) return;  // ALT0 = GPIO
  const gpio_pin_config_t input = {kGPIO_DigitalInput, 0U};
  GPIO_PinInit(s_gpios[pin / 32U], pin % 32U, &input);
}

bool io_pin_read(uint8_t pin) {
  if (pin / 32U >= COUNT_OF(s_gpios)) return true;  // reads HIGH, like an unconnected pin with a pull-up
  return GPIO_PinRead(s_gpios[pin / 32U], pin % 32U) != 0U;
}

void io_pin_output(uint8_t pin, bool high) {
  if (!setup_pin(pin, kPORT_MuxAlt0, false, true)) return;
  const gpio_pin_config_t output = {kGPIO_DigitalOutput, high ? 1U : 0U};
  GPIO_PinInit(s_gpios[pin / 32U], pin % 32U, &output);
}

void io_pin_write(uint8_t pin, bool high) {
  if (pin / 32U >= COUNT_OF(s_gpios)) return;
  GPIO_PinWrite(s_gpios[pin / 32U], pin % 32U, high ? 1U : 0U);
}

void io_delay_us(uint32_t us) { SDK_DelayAtLeastUs(us, SystemCoreClock); }

// ---------------------------------------------------------------- analog inputs (ADC1)

// Arduino analog inputs A0..A5 of the FRDM-MCXA156: their pin and ADC1 channel.
static const struct {
  uint8_t pin;
  uint8_t channel;
} s_analog[] = {
    {MCX_PIN(1, 10), 8U},   // A0 = P1_10 = ADC1_A8
    {MCX_PIN(2, 5), 1U},    // A1 = P2_5  = ADC1_A1
    {MCX_PIN(2, 3), 4U},    // A2 = P2_3  = ADC1_A4
    {MCX_PIN(2, 4), 0U},    // A3 = P2_4  = ADC1_A0
    {MCX_PIN(1, 12), 10U},  // A4 = P1_12 = ADC1_A10
    {MCX_PIN(1, 13), 11U},  // A5 = P1_13 = ADC1_A11
};
static bool s_adcReady;
static float s_fullScaleMv;
static uint8_t s_analogPinsReady;  // bit i set: input i has its pin set up as analog

static void init_adc(void) {
  RESET_ReleasePeripheralReset(kADC1_RST_SHIFT_RSTn);
  CLOCK_AttachClk(kFRO12M_to_ADC1);
  CLOCK_SetClockDiv(kCLOCK_DivADC1, 1U);

  lpadc_config_t config;
  LPADC_GetDefaultConfig(&config);
  config.powerLevelMode = kLPADC_PowerLevelAlt4;
  config.enableAnalogPreliminary = true;
  config.referenceVoltageSource = kLPADC_ReferenceVoltageAlt3;  // VDDA, the 3.3 V supply
  LPADC_Init(ADC1, &config);
  LPADC_DoOffsetCalibration(ADC1);
  SDK_DelayAtLeastUs(1U, SystemCoreClock);
  LPADC_DoAutoCalibration(ADC1);

  lpadc_conv_trigger_config_t trigger;
  LPADC_GetDefaultConvTriggerConfig(&trigger);
  trigger.targetCommandId = 1U;  // a software trigger 0 runs command 1
  trigger.enableHardwareTrigger = false;
  LPADC_SetConvTriggerConfig(ADC1, 0U, &trigger);
  s_adcReady = true;
}

bool io_analog_mv(uint8_t input, float* mv) {
  if (!s_adcReady || input >= COUNT_OF(s_analog)) return false;
  if ((s_analogPinsReady & (1U << input)) == 0U) {
    setup_pin(s_analog[input].pin, kPORT_MuxAlt0, false, false);  // analog: no pull-up, digital input off
    s_analogPinsReady |= (uint8_t)(1U << input);
  }

  lpadc_conv_command_config_t command;
  LPADC_GetDefaultConvCommandConfig(&command);
  command.channelNumber = s_analog[input].channel;
  command.conversionResolutionMode = kLPADC_ConversionResolutionHigh;  // 16 bits
  command.hardwareAverageMode = kLPADC_HardwareAverageCount4;
  LPADC_SetConvCommandConfig(ADC1, 1U, &command);
  LPADC_DoSoftwareTrigger(ADC1, 1U);  // bit 0: trigger 0

  lpadc_conv_result_t result;
  uint32_t tries = 0;
  while (!LPADC_GetConvResult(ADC1, &result)) {
    if (++tries > 100000U) return false;  // the converter never finished
  }
  *mv = result.convValue * s_fullScaleMv / 65536.0f;
  return true;
}

bool mcx_io_burst_mv(uint8_t input, float* mv, int n, uint32_t periodUs, uint32_t* spanUs) {
  if (!s_adcReady || input >= COUNT_OF(s_analog) || mv == NULL || n < 2) return false;
  if ((s_analogPinsReady & (1U << input)) == 0U) {
    setup_pin(s_analog[input].pin, kPORT_MuxAlt0, false, false);
    s_analogPinsReady |= (uint8_t)(1U << input);
  }
  lpadc_conv_command_config_t command;
  LPADC_GetDefaultConvCommandConfig(&command);
  command.channelNumber = s_analog[input].channel;
  command.conversionResolutionMode = kLPADC_ConversionResolutionHigh;  // 16 bits
  command.hardwareAverageMode = kLPADC_HardwareAverageCount1;          // one conversion per reading: fast
  LPADC_SetConvCommandConfig(ADC1, 1U, &command);

  uint32_t next = bench_micros();
  uint32_t first = next, last = next;
  for (int i = 0; i < n; ++i) {
    while ((int32_t)(bench_micros() - next) < 0) {
    }  // wait for this reading's time
    last = bench_micros();
    if (i == 0) first = last;
    next += periodUs;
    LPADC_DoSoftwareTrigger(ADC1, 1U);
    lpadc_conv_result_t result;
    uint32_t tries = 0;
    while (!LPADC_GetConvResult(ADC1, &result)) {
      if (++tries > 100000U) return false;
    }
    mv[i] = result.convValue * s_fullScaleMv / 65536.0f;
  }
  *spanUs = last - first;
  return true;
}

// ---------------------------------------------------------------- I2C (LPI2C0)

static bool s_i2cReady;

static void init_i2c(void) {
  RESET_ReleasePeripheralReset(kLPI2C0_RST_SHIFT_RSTn);
  CLOCK_AttachClk(kFRO12M_to_LPI2C0);
  CLOCK_SetClockDiv(kCLOCK_DivLPI2C0, 1U);
  setup_pin(MCX_PIN(0, 16), kPORT_MuxAlt2, true, true);  // D14 = SDA
  setup_pin(MCX_PIN(0, 17), kPORT_MuxAlt2, true, true);  // D15 = SCL

  lpi2c_master_config_t config;
  LPI2C_MasterGetDefaultConfig(&config);
  config.baudRate_Hz = 100000U;
  LPI2C_MasterInit(LPI2C0, &config, CLOCK_GetLpi2cClkFreq(0U));
  s_i2cReady = true;
}

bool io_i2c_write(uint8_t address, const uint8_t* data, size_t length) {
  if (!s_i2cReady) return false;
  lpi2c_master_transfer_t transfer;
  memset(&transfer, 0, sizeof transfer);
  transfer.slaveAddress = address;
  transfer.direction = kLPI2C_Write;
  transfer.data = (void*)data;
  transfer.dataSize = length;
  transfer.flags = kLPI2C_TransferDefaultFlag;
  return LPI2C_MasterTransferBlocking(LPI2C0, &transfer) == kStatus_Success;
}

// The bytes in `out` (a register number) go first, then the read, in one transfer (repeated start).
bool io_i2c_write_read(uint8_t address, const uint8_t* out, size_t outLength, uint8_t* in, size_t inLength) {
  if (!s_i2cReady || outLength == 0U || outLength > 4U || inLength == 0U) return false;
  lpi2c_master_transfer_t transfer;
  memset(&transfer, 0, sizeof transfer);
  transfer.slaveAddress = address;
  transfer.direction = kLPI2C_Read;
  for (size_t i = 0; i < outLength; ++i) transfer.subaddress = (transfer.subaddress << 8) | out[i];
  transfer.subaddressSize = outLength;
  transfer.data = in;
  transfer.dataSize = inLength;
  transfer.flags = kLPI2C_TransferDefaultFlag;
  return LPI2C_MasterTransferBlocking(LPI2C0, &transfer) == kStatus_Success;
}

// ---------------------------------------------------------------- SPI (bit-banged)

// The RC522 needs only a few hundred register accesses a second, so plain GPIO is fast enough and any pins
// will do. Mode 0: MOSI changes while SCK is low, MISO is read just after SCK rises.
static bool s_spiReady;
static int s_csPin = -1;  // the chip-select pin set up last

bool io_spi_transfer(uint8_t csPin, const uint8_t* out, uint8_t* in, size_t length) {
  if (!s_spiReady) {
    io_pin_output(MCX_SPI_SCK, false);
    io_pin_output(MCX_SPI_MOSI, false);
    io_pin_input_pullup(MCX_SPI_MISO);  // with nothing connected every byte reads 0xFF: "no chip"
    s_spiReady = true;
  }
  if (csPin != s_csPin) {
    io_pin_output(csPin, true);  // chip select is HIGH between transfers
    s_csPin = csPin;
  }
  io_pin_write(csPin, false);
  for (size_t i = 0; i < length; ++i) {
    uint8_t send = out[i];
    uint8_t got = 0;
    for (int bit = 7; bit >= 0; --bit) {
      io_pin_write(MCX_SPI_MOSI, ((send >> bit) & 1U) != 0U);
      io_pin_write(MCX_SPI_SCK, true);
      got = (uint8_t)((got << 1) | (io_pin_read(MCX_SPI_MISO) ? 1U : 0U));
      io_pin_write(MCX_SPI_SCK, false);
    }
    if (in != NULL) in[i] = got;
  }
  io_pin_write(csPin, true);
  return true;
}


// ---------------------------------------------------------------- start

void mcx_io_init(float adcFullScaleMv) {
  s_fullScaleMv = adcFullScaleMv;
  init_adc();
  init_i2c();
}