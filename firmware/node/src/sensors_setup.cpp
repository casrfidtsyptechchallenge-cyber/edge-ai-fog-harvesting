#include "sensors_setup.h"

#include <Arduino.h>
#include <Wire.h>

#include "node_config.h"

// -----------------------------------------------------------------------------------------------
// TO ADD A SENSOR
//   1. Write src/sensors/<name>_sensor.h/.cpp: a class that implements Sensor (see sensor.h).
//   2. In node_config.h, add AEROION_HAS_<NAME> plus its pins and calibration.
//   3. Below, add a block "#if AEROION_HAS_<NAME> ... #endif" that creates the object, and one
//      registry.add() line in registerSensors(). If it fills a new telemetry field, add the field
//      to SensorFrame and to telemetry_json.cpp (and to the contract first).
// TO REMOVE A SENSOR
//   Set its AEROION_HAS_<NAME> to 0 in node_config.h. Its telemetry fields become null.
// -----------------------------------------------------------------------------------------------

#if AEROION_FAKE_SENSORS
#include "sensors/fake_sensor.h"
static FakeSensor fakeSensor;
#endif

#if AEROION_HAS_SHT45
#include "sensors/sht45_sensor.h"
static Sht45Sensor sht45(Wire);
#endif

#if AEROION_HAS_HX711
#include "sensors/hx711_sensor.h"
static Hx711Sensor loadCell(PIN_HX711_DOUT, PIN_HX711_SCK, HX711_COUNTS_PER_GRAM);
#endif

#if AEROION_HAS_INA219
#include "sensors/ina219_sensor.h"
static Ina219Sensor hvPower(Wire, INA219_SHUNT_OHMS);
#endif

#if AEROION_HAS_OPTICAL_FOG
#include "sensors/optical_fog_sensor.h"
static OpticalFogSensor opticalFog(PIN_FOG_LED, PIN_FOG_PHOTO, FOG_CLEAR_MV, FOG_DENSE_MV);
#endif

#if AEROION_HAS_COLLECTOR_CURRENT
#include "sensors/collector_current_sensor.h"
static CollectorCurrentSensor collectorCurrent(PIN_COLLECTOR_I, COLLECTOR_SHUNT_OHMS, COLLECTOR_AMP_GAIN);
#endif

#if AEROION_HAS_RADAR
#include "sensors/presence_radar_sensor.h"
static PresenceRadarSensor radar(PIN_RADAR_OUT);
#endif

#if AEROION_HAS_LID_SWITCH
#include "sensors/lid_switch_sensor.h"
static LidSwitchSensor lidSwitch(PIN_LID_SWITCH);
#endif


#if AEROION_HAS_FOG_KNOB && AEROION_HAS_OPTICAL_FOG
#error "AEROION_HAS_FOG_KNOB and AEROION_HAS_OPTICAL_FOG both fill fog_idx_opt: enable only one"
#endif
#if AEROION_HAS_FOG_KNOB
#include "sensors/fog_knob_sensor.h"
static FogKnobSensor fogKnob(PIN_FOG_KNOB);
#endif

void registerSensors(SensorRegistry& registry) {
#if AEROION_FAKE_SENSORS
  registry.add(&fakeSensor);  // the fake sensor replaces all real ones
#else
#if AEROION_HAS_SHT45
  registry.add(&sht45);
#endif
#if AEROION_HAS_HX711
  registry.add(&loadCell);
#endif
#if AEROION_HAS_INA219
  registry.add(&hvPower);
#endif
#if AEROION_HAS_OPTICAL_FOG
  registry.add(&opticalFog);
#endif
#if AEROION_HAS_COLLECTOR_CURRENT
  registry.add(&collectorCurrent);
#endif
#if AEROION_HAS_RADAR
  registry.add(&radar);
#endif
#if AEROION_HAS_LID_SWITCH
  registry.add(&lidSwitch);
#endif
#if AEROION_HAS_FOG_KNOB
  registry.add(&fogKnob);
#endif
#endif
}

void printRawReadings() {
#if !AEROION_FAKE_SENSORS
#if AEROION_HAS_HX711
  Serial.printf("# raw HX711 %ld counts\n", loadCell.lastRaw());
#endif
#if AEROION_HAS_INA219
  Serial.printf("# raw INA219 %.3f V %.2f mA\n", hvPower.busVolts(), hvPower.currentMa());
#endif
#if AEROION_HAS_OPTICAL_FOG
  Serial.printf("# raw OPTICAL-FOG signal %.0f mV (clear %.0f, dense %.0f)\n", opticalFog.lastSignalMv(),
                FOG_CLEAR_MV, FOG_DENSE_MV);
#endif
#if AEROION_HAS_COLLECTOR_CURRENT
  Serial.printf("# raw COLLECTOR-I %.1f mV (zero %.1f mV)\n", collectorCurrent.lastMv(), collectorCurrent.zeroMv());
#endif
#endif
}