#pragma once
// Settings for the ESP32-S3 of ONE two-chip node (M7). The node's MCX A156 has its own file:
// firmware/mcx_node/source/mcx_node_config.h. The two must have the same NODE_ID.

// ---------------- Identity ----------------
#ifndef LINK_NODE_ID
#define LINK_NODE_ID       "n01"        // the same as NODE_ID on the MCX of this node
#endif
#define LINK_FW_VERSION    "0.5.0-esp"  // shown next to the MCX's version in the info message

// ---------------- Network ----------------
#if AEROION_WOKWI
// The simulator (env esp32s3-link-wokwi): its open access point, and the PC through the IoT gateway built
// into Wokwi for VS Code.
#define WIFI_SSID          "Wokwi-GUEST"
#define WIFI_PASSWORD      ""
#define WIFI_CHANNEL       6            // known channel: joins about 4 s faster than with a scan
#define MQTT_HOST          "host.wokwi.internal"
#define LINK_LOG_TO_MCX_PORT 1          // Wokwi's serial monitor is on the MCX pins: show the log there too
#define LINK_RADIO_ON_LINK 1            // Wokwi has no radio between boards: FOG_ALERT packets travel on the link
#else
// A real board (env esp32s3-link): your network, in a file that never goes to the repository.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#error "Copy include/wifi_secrets.example.h to include/wifi_secrets.h and fill in your Wi-Fi and broker"
#endif
#endif

#ifndef MQTT_PORT
#define MQTT_PORT          1883
#endif
#ifndef MQTT_USER
#define MQTT_USER          ""           // empty: the broker allows anonymous clients (bench setup)
#define MQTT_PASSWORD      ""
#endif
#ifndef NTP_SERVER
#define NTP_SERVER         "pool.ntp.org"  // a site without Internet: run an NTP server on the Pi, put its address
#endif
#ifndef LINK_LOG_TO_MCX_PORT
#define LINK_LOG_TO_MCX_PORT 0
#endif
#ifndef LINK_RADIO_ON_LINK
#define LINK_RADIO_ON_LINK 0
#endif
#define MQTT_RETRY_MS      5000         // time between two connection attempts
#define MQTT_BUFFER_BYTES  4608         // the largest message: a 64-badge SYNC_ALLOWLIST, about 3.5 kB

// ---------------- UART link to the MCX (docs/link.md) ----------------
#define LINK_SERIAL        Serial1
#define PIN_LINK_RX        18           // from the MCX's D1 (TX)
#define PIN_LINK_TX        17           // to the MCX's D0 (RX)
#define LINK_BAUD          115200       // the same as LINK_BAUD in mcx_node_config.h

// ---------------- The ESP32's own sensors: 1 = installed, 0 = not installed ----------------
#define LINK_HAS_HX711     1            // load cell under the collector (TARE is done here)
#if AEROION_WOKWI
#define LINK_HAS_SHT45     0            // Wokwi has no SHT45: t_c and rh_pct stay null
#else
#define LINK_HAS_SHT45     1            // temperature + humidity at the inlet (I2C)
#endif
#define PIN_I2C_SDA        8
#define PIN_I2C_SCL        9
#define PIN_HX711_DOUT     4
#define PIN_HX711_SCK      5
#define HX711_COUNTS_PER_GRAM 420.0f   // calibrate as in node_config.h

// ---------------- Timing ----------------
#define READINGS_PERIOD_MS 1000         // the ESP32's values given to the gateway
#define STATUS_PRINT_MS    60000        // a status summary in the log
#define LINK_WATCHDOG_MS   20000        // connecting to Wi-Fi or MQTT can block a few seconds