#pragma once
#include "sensor_registry.h"

// The ONE place that knows which sensors exist. See sensors_setup.cpp for how to add or remove one.
void registerSensors(SensorRegistry& registry);

// Calibration helper: prints the raw value of each installed sensor as '#' lines.
// Called once per second when AEROION_DEBUG_RAW is 1 in node_config.h.
void printRawReadings();