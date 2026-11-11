// Shared by the harnesses whose driver reads the pack from the datalayer (inverters, RS485 devices,
// chargers, shunts).
#pragma once
#include "src/datalayer/datalayer.h"

// What a running 96s NMC pack looks like to an inverter driver once a battery integration has
// filled the datalayer.
static inline void populate_battery_state() {
  auto& info = datalayer.battery.info;
  auto& st = datalayer.battery.status;
  info.total_capacity_Wh = 30000;
  info.reported_total_capacity_Wh = 30000;
  info.max_design_voltage_dV = 4040;
  info.min_design_voltage_dV = 2450;
  info.max_cell_voltage_mV = 4200;
  info.min_cell_voltage_mV = 2800;
  info.number_of_cells = 96;
  st.voltage_dV = 3700;
  st.current_dA = 100;
  st.reported_current_dA = 100;
  st.real_soc = 5000;
  st.reported_soc = 5000;
  st.soh_pptt = 9900;
  st.remaining_capacity_Wh = 15000;
  st.reported_remaining_capacity_Wh = 15000;
  st.max_charge_power_W = 5000;
  st.max_discharge_power_W = 5000;
  st.max_charge_current_dA = 150;
  st.max_discharge_current_dA = 150;
  st.cell_max_voltage_mV = 3720;
  st.cell_min_voltage_mV = 3680;
  st.temperature_max_dC = 250;
  st.temperature_min_dC = 220;
  st.active_power_W = 3700;
  for (int i = 0; i < 96; i++) {
    st.cell_voltages_mV[i] = 3700;
  }
}
