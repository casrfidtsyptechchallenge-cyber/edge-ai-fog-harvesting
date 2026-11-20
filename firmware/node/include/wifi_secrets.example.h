#pragma once
// Copy this file to include/wifi_secrets.h and fill it in. wifi_secrets.h is in .gitignore: your Wi-Fi
// password never goes to the repository. Used by env esp32s3-link (the real ESP32-S3 of a two-chip node).

#define WIFI_SSID      "---"
#define WIFI_PASSWORD  "---"
#define WIFI_CHANNEL   0                // 0 = find the network's channel
#define MQTT_HOST      "---"   // the PC or Raspberry Pi running mosquitto (its IP on your network)

// Optional:
// #define MQTT_PORT      1883
// #define MQTT_USER      "node01"       // only if the broker asks for a password
// #define MQTT_PASSWORD  "..."
// #define NTP_SERVER     "192.168.1.20" // a site without Internet: the Pi's NTP server