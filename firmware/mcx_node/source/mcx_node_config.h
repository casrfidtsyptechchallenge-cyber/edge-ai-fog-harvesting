#pragma once
// Settings for the MCX A156 of ONE node, the safety controller: identity, installed sensors, pins and
// calibration. The node's ESP32-S3 has its own file: firmware/node/include/node_config.h.
#include "mcx_io.h"

// ---------------- Identity ----------------
#define NODE_ID          "n01"   // the same id as this node's ESP32: the two chips are one node
#define NODE_FW_VERSION  "0.5.0-mcx"
#define NODE_POS_X_M     0.0f    // position on the site map in metres (+x = east, +y = north)
#define NODE_POS_Y_M     0.0f

// ---------------- Installed sensors: 1 = installed, 0 = not installed ----------------
// A sensor that is not installed, or does not answer, reports null in the telemetry; nothing else changes.
#define AEROION_HAS_COLLECTOR_CURRENT 1   // shunt + op-amp on the collector's ground return
#define AEROION_HAS_OPTICAL_FOG       0   // IR LED + photodiode across the box
#define AEROION_HAS_FOG_KNOB          1   // bench stand-in: a potentiometer plays the fog (not with OPTICAL_FOG)
#define AEROION_HAS_INA219            1   // power drawn by the HV module (I2C)
#define AEROION_HAS_RADAR             1   // LD2410 presence radar, OUT pin
#define AEROION_HAS_LID_SWITCH        1   // enclosure lid switch
#define AEROION_HAS_RC522             1   // RFID badge reader (M4); while missing, SW2/SW3 still act as badges

// ---------------- Timing ----------------
#define TELEMETRY_PERIOD_MS  1000
#define STATUS_PERIOD_MS     10000
#define SAFETY_PERIOD_MS     50      // how often the safety core (M3) runs
#define WATCHDOG_TIMEOUT_MS  3000    // the main loop must run at least this often, or the chip restarts

// ---------------- Station link (M7): the ESP32 on D0/D1, or tools/serial_bridge.py on USB ----------------
#define LINK_BAUD        115200  // UART to the ESP32; its firmware must use the same speed
#define LINK_SILENT_MS   15000   // no time line for this long: FAULT COMM_LOST (the link sends one every 5 s)
#define LINK_LINE_MAX    4095    // longest line taken from the PC or the ESP32 (64 badges: about 3 kB)

// ---------------- High voltage and safety (M3) ----------------
#define HV_DEFAULT_LEVEL       0.60f   // level asked for in HARVEST until the controller (M6) sends requests
#define SAFETY_ARC_CURRENT_UA  250.0f  // collector current that counts as an arc
#define AEROION_HAS_HV_OUTPUT  0       // 1 once the HV stage is wired: turns on the check below
#define SAFETY_MIN_CURRENT_UA  0.3f    // with the HV on at level >= 0.45, less than this for 3 s: SENSOR_FAIL
                                       // (a disconnected sensor reads ~0). Measure the real current first.

// ---------------- Arc predictor (M5a) ----------------
#define NOVEL_WAVE_GAP_MS 10000   // at most one NOVEL wave (a window unlike the training data) every 10 s
#define ARC_PERIOD_MS     100     // a 25.6 ms window of collector current every 100 ms while the HV is allowed
#define ARC_WARN_RISK     0.30f   // risk (probability of an arc within 5 s) above which the node backs off.
                                  // 0.30: 60 % fewer arcs, almost no faults, 3x the water (docs/edge_ai.md)
#define ARC_WAVE_EVERY_S  0       // also send a window as a "wave" message every N s (training data); 0 = off

// ---------------- Pins: Arduino header of the FRDM-MCXA156 ----------------
#define PIN_COLLECTOR_I  MCX_A0          // P1_10
#define PIN_FOG_PHOTO    MCX_A1          // P2_5
#define PIN_FOG_KNOB     PIN_FOG_PHOTO   // the bench knob uses the photodiode's input
#define PIN_FOG_LED      MCX_D2          // P3_1
#define PIN_RADAR_OUT    MCX_D4          // P3_31
#define PIN_LID_SWITCH   MCX_D8          // P1_15
#define PIN_RC522_CS     MCX_D9          // P3_17, the pin marked SDA on the RC522 module
#define PIN_RC522_RESET  MCX_D5          // P3_14, the pin marked RST
// INA219: I2C on D14 (SDA) and D15 (SCL). RC522: SPI on D13 (SCK), D11 (MOSI), D12 (MISO). See mcx_io.h.
// D0 (RX) and D1 (TX): UART link to the ESP32 (M7), see mcx_uart.h.
// Keep free for later: D6/D7 (HV level PWM and HV enable).
// Avoid D3 and D10: they also drive the board's red and green LED.

// ---------------- Badges (M4) ----------------
// The node's own badge list, used until the station sends its list (SYNC_ALLOWLIST) and after every restart.
// Put the real operator and technician cards here too, so they work even while the station is down.
// UID: 8 to 20 upper-case hex characters, as the node prints it when a card is read ("# badge 04A1B2C3 ...").
// The first two are the test badges of the buttons SW2 (operator) and SW3 (technician).
#define AEROION_LOCAL_BADGES                \
  {"0A0B0C0D", AEROION_ROLE_OPERATOR},      \
  {"0E0E0E0E", AEROION_ROLE_TECHNICIAN},

// ---------------- Calibration: measure these on your own hardware ----------------
#define ADC_FULL_SCALE_MV       3300.0f   // the board's 3.3 V supply (ADC reference); measure it for more accuracy
#define INA219_SHUNT_OHMS       0.1f      // shunt resistor on the INA219 board ("R100" = 0.1 ohm)
#define FOG_CLEAR_MV            1800.0f   // photodiode signal (LED on - LED off) in clear air
#define FOG_DENSE_MV            200.0f    // the same signal in the densest fog of your chamber
#define COLLECTOR_SHUNT_OHMS    1000.0f   // shunt between the collector and ground
#define COLLECTOR_AMP_GAIN      10.0f     // gain of the op-amp after the shunt