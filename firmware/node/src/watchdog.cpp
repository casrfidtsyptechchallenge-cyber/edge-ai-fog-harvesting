#include "watchdog.h"

#include <esp_idf_version.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

void watchdogBegin(uint32_t timeoutMs) {
  // The watchdog functions changed in ESP-IDF 5 (Arduino-ESP32 3.x); this works with both.
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t config = {};
  config.timeout_ms = timeoutMs;
  config.idle_core_mask = 0;    // watch only the tasks added below
  config.trigger_panic = true;  // restart the chip
  if (esp_task_wdt_reconfigure(&config) != ESP_OK) esp_task_wdt_init(&config);
#else
  esp_task_wdt_init((timeoutMs + 999) / 1000, true);  // seconds; true = restart the chip
#endif
  esp_task_wdt_add(nullptr);  // nullptr = this task, the one running loop()
}

void watchdogFeed() { esp_task_wdt_reset(); }

bool abnormalRestart(aeroion_fault_t* code, const char** detail) {
  switch (esp_reset_reason()) {
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
      *code = AEROION_FAULT_WATCHDOG;
      *detail = "restarted by the watchdog";
      return true;
    case ESP_RST_PANIC:
      *code = AEROION_FAULT_OTHER;
      *detail = "restarted after a crash";
      return true;
    case ESP_RST_BROWNOUT:
      *code = AEROION_FAULT_OTHER;
      *detail = "restarted after a brown-out (supply voltage too low)";
      return true;
    default:
      return false;
  }
}