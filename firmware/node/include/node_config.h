#pragma once
// Settings for ONE node. Each node gets its own copy: id, position, installed sensors, calibration.

// ---------------- Identity ----------------
#define NODE_ID          "n01"
#define NODE_FW_VERSION  "0.2.0-m3"
#define NODE_POS_X_M     0.0f    // position on the site map in metres (+x = east, +y = north)
#define NODE_POS_Y_M     0.0f

// ---------------- Installed sensors: 1 = installed, 0 = not installed ----------------
// A sensor that is not installed reports null in the telemetry; nothing else changes.
#define AEROION_FAKE_SENSORS          0   // 1 = no hardware: one fake sensor fills every field
#define AEROION_HAS_SHT45             0   // temperature + humidity (I2C)
#define AEROION_HAS_HX711             1   // load cell under the collector
#define AEROION_HAS_INA219            0   // power drawn by the HV module (I2C)
#define AEROION_HAS_OPTICAL_FOG       0   // IR LED + photodiode across the box
#define AEROION_HAS_COLLECTOR_CURRENT 1   // shunt + op-amp on the collector's ground return
#define AEROION_HAS_RADAR             1   // LD2410 presence radar, OUT pin
#define AEROION_HAS_LID_SWITCH        1   // enclosure lid switch
#define AEROION_HAS_FOG_KNOB          1   // Wokwi only : a potentiometer plays the fog (not with OPTICAL_FOG) 
// ---------------- Timing ----------------
#define TELEMETRY_PERIOD_MS  1000
#define STATUS_PERIOD_MS     10000
#define SAFETY_PERIOD_MS     50      // how often the safety core (M3) runs
#define WATCHDOG_TIMEOUT_MS  3000    // loop() must run at least this often, or the ESP32 restarts

// ---------------- High voltage and safety (M3) ----------------
#define HV_DEFAULT_LEVEL       0.60f   // level asked for in HARVEST until the controller (M6) exists
#define SAFETY_ARC_CURRENT_UA  250.0f  // collector current that counts as an arc (see the shunt and gain below)
#define AEROION_BENCH_CONSOLE  1       // 1 = serial commands (op, tech, lock...) for tests; 0 on field nodes

// ---------------- Debug ----------------
#define AEROION_DEBUG_RAW    1   // 1 = also print raw readings as '#' lines, for calibration

// ---------------- Pins ----------------
// Analog inputs must be on ADC1: ADC2 cannot be read while Wi-Fi is on (needed in M7).
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define PIN_I2C_SDA      8
#define PIN_I2C_SCL      9
#define PIN_HX711_DOUT   4
#define PIN_HX711_SCK    5
#define PIN_FOG_LED      6
#define PIN_FOG_PHOTO    1    // ADC1 channel 0
#define PIN_COLLECTOR_I  2    // ADC1 channel 1
#define PIN_RADAR_OUT    7
#define PIN_LID_SWITCH   10
#define PIN_HV_ENABLE    11   // MOSFET that switches the HV module's 12 V supply (10k pull-down on the gate)
#define PIN_HV_LEVEL     12   // PWM -> RC filter -> HV module control input
#else  // classic ESP32 DevKit (esp32dev)
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
#define PIN_HX711_DOUT   16
#define PIN_HX711_SCK    17
#define PIN_FOG_LED      25
#define PIN_FOG_PHOTO    34   // ADC1 channel 6 (input only)
#define PIN_COLLECTOR_I  35   // ADC1 channel 7 (input only)
#define PIN_RADAR_OUT    27
#define PIN_LID_SWITCH   26
#define PIN_HV_ENABLE    32   // MOSFET that switches the HV module's 12 V supply (10k pull-down on the gate)
#define PIN_HV_LEVEL     33   // PWM -> RC filter -> HV module control input
#endif

#define PIN_FOG_KNOB     PIN_FOG_PHOTO   // the Wokwi fog knob uses the photodiode's pin

// ---------------- Calibration: measure these on your own hardware ----------------
#define HX711_COUNTS_PER_GRAM   420.0f    // (raw with a known weight - raw empty) / weight in grams
#define INA219_SHUNT_OHMS       0.1f      // shunt resistor on the INA219 board ("R100" = 0.1 ohm)
#define FOG_CLEAR_MV            1800.0f   // photodiode signal (LED on - LED off) in clear air
#define FOG_DENSE_MV            200.0f    // the same signal in the densest fog of your chamber
#define COLLECTOR_SHUNT_OHMS    1000.0f   // shunt between the collector and ground
#define COLLECTOR_AMP_GAIN      10.0f     // gain of the op-amp after the shunt