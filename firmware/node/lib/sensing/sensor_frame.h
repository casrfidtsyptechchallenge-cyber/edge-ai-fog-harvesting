#pragma once
#include "nullable.h"

// The latest sensor values, in the units of the contract (docs/contract-reference.md).
// Every field starts empty (null); each installed sensor fills only its own fields.
struct SensorFrame {
  Nullable<float> fogIdxOpt;      // fog_idx_opt, 0..1     optical fog sensor
  Nullable<float> tempC;          // t_c, °C               SHT45
  Nullable<float> rhPct;          // rh_pct, %             SHT45
  Nullable<float> massG;          // mass_g, g             load cell
  Nullable<float> rateGpm;        // rate_gpm, g/min       load cell
  Nullable<float> currentUa;      // i_ua, µA              collector current
  Nullable<float> powerW;         // p_w, W                INA219
  Nullable<float> energyWh;       // e_wh, Wh              INA219
  Nullable<float> battPct;        // batt_pct, %           battery monitor (not built yet)
  Nullable<bool> radarPresence;   // radar_presence        radar
  Nullable<bool> lidClosed;       // lid_closed            lid switch

  void clear() { *this = SensorFrame(); }
};