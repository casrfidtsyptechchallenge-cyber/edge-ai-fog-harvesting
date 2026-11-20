#include "node_sensors.h"

#include <stdio.h>

namespace {
// Integers only: the small C library on the chip cannot print floats.
long rounded(float value) { return static_cast<long>(value >= 0.0f ? value + 0.5f : value - 0.5f); }
}  // namespace

void NodeSensors::registerAll(SensorRegistry& registry) {
#if AEROION_HAS_COLLECTOR_CURRENT
  registry.add(&collectorCurrent);
#endif
#if AEROION_HAS_OPTICAL_FOG
  registry.add(&opticalFog);
#endif
#if AEROION_HAS_FOG_KNOB
  registry.add(&fogKnob);
#endif
#if AEROION_HAS_INA219
  registry.add(&hvPower);
#endif
#if AEROION_HAS_RADAR
  registry.add(&radar);
#endif
#if AEROION_HAS_LID_SWITCH
  registry.add(&lidSwitch);
#endif
#if AEROION_HAS_RC522
  registry.add(&badgeReader);
#endif
  (void)registry;  // used only if a sensor is enabled
}

void NodeSensors::printRaw(PrintFn print) const {
  char text[96];
#if AEROION_HAS_COLLECTOR_CURRENT
  snprintf(text, sizeof text, "# raw COLLECTOR-I %ld mV (zero %ld mV)\r\n", rounded(collectorCurrent.lastMv()),
           rounded(collectorCurrent.zeroMv()));
  print(text);
#endif
#if AEROION_HAS_OPTICAL_FOG
  snprintf(text, sizeof text, "# raw OPTICAL-FOG signal %ld mV (clear %ld, dense %ld)\r\n",
           rounded(opticalFog.lastSignalMv()), rounded(FOG_CLEAR_MV), rounded(FOG_DENSE_MV));
  print(text);
#endif
#if AEROION_HAS_FOG_KNOB
  snprintf(text, sizeof text, "# raw FOG-KNOB %ld mV (full scale %ld mV)\r\n", rounded(fogKnob.lastMv()),
           rounded(ADC_FULL_SCALE_MV));
  print(text);
#endif
#if AEROION_HAS_INA219
  snprintf(text, sizeof text, "# raw INA219 %ld mV %ld mA\r\n", rounded(hvPower.busVolts() * 1000.0f),
           rounded(hvPower.currentMa()));
  print(text);
#endif
#if AEROION_HAS_RC522
  snprintf(text, sizeof text, "# raw RC522 chip version 0x%02X (0x91 or 0x92 = NXP; 0x00 or 0xFF = no chip)\r\n",
           badgeReader.chipVersion());
  print(text);
#endif
  (void)text;
  (void)print;
}

bool NodeSensors::zeroCurrent() {
#if AEROION_HAS_COLLECTOR_CURRENT
  return collectorCurrent.zero();
#else
  return false;
#endif
}

bool NodeSensors::takeBadgeTap(char* uid, size_t cap) {
#if AEROION_HAS_RC522
  return badgeReader.takeTap(uid, cap);
#else
  (void)uid;
  (void)cap;
  return false;
#endif
}