#pragma once
#include <stddef.h>

#include "mcx_node_config.h"
#include "sensor_registry.h"

#if AEROION_HAS_COLLECTOR_CURRENT
#include "collector_current_driver.h"
#endif
#if AEROION_HAS_OPTICAL_FOG || AEROION_HAS_FOG_KNOB
#include "optical_fog_driver.h"
#endif
#if AEROION_HAS_INA219
#include "ina219_driver.h"
#endif
#if AEROION_HAS_RADAR || AEROION_HAS_LID_SWITCH
#include "switch_drivers.h"
#endif
#if AEROION_HAS_RC522
#include "rc522_reader.h"
#endif

#if AEROION_HAS_FOG_KNOB && AEROION_HAS_OPTICAL_FOG
#error "AEROION_HAS_FOG_KNOB and AEROION_HAS_OPTICAL_FOG both fill fog_idx_opt: enable only one"
#endif

// The sensors wired to this MCX, as chosen in mcx_node_config.h. The RFID reader is one of them: the
// registry sets it up and looks for it again when it is missing, like any sensor.
//
// TO ADD A SENSOR
//   1. Write its driver in firmware/node/lib/drivers: a class that implements Sensor (sensor.h) and touches
//      the hardware only through board_io.h. Add a test for it in firmware/node/test/test_drivers.
//   2. In mcx_node_config.h, add AEROION_HAS_<NAME> plus its pins and calibration.
//   3. Here: a member with its settings and one line in registerAll(); in CMakeLists.txt: its .cpp file.
// TO REMOVE A SENSOR
//   Set its AEROION_HAS_<NAME> to 0 in mcx_node_config.h. Its telemetry fields become null.
struct NodeSensors {
#if AEROION_HAS_COLLECTOR_CURRENT
  CollectorCurrentDriver collectorCurrent{PIN_COLLECTOR_I, COLLECTOR_SHUNT_OHMS, COLLECTOR_AMP_GAIN};
#endif
#if AEROION_HAS_OPTICAL_FOG
  OpticalFogDriver opticalFog{PIN_FOG_LED, PIN_FOG_PHOTO, FOG_CLEAR_MV, FOG_DENSE_MV};
#endif
#if AEROION_HAS_FOG_KNOB
  FogKnobDriver fogKnob{PIN_FOG_KNOB, ADC_FULL_SCALE_MV};
#endif
#if AEROION_HAS_INA219
  Ina219Driver hvPower{INA219_SHUNT_OHMS};
#endif
#if AEROION_HAS_RADAR
  RadarPinDriver radar{PIN_RADAR_OUT};
#endif
#if AEROION_HAS_LID_SWITCH
  LidPinDriver lidSwitch{PIN_LID_SWITCH};
#endif
#if AEROION_HAS_RC522
  Rc522Reader badgeReader{PIN_RC522_CS, PIN_RC522_RESET};
#endif

  typedef void (*PrintFn)(const char* text);

  void registerAll(SensorRegistry& registry);
  void printRaw(PrintFn print) const;      // the raw readings, for calibration
  bool zeroCurrent();                      // measure the collector-current zero again; only with the HV off
  bool takeBadgeTap(char* uid, size_t cap);  // a card just presented to the reader, once (false: none)
};