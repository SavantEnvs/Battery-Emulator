// Feeds a stream of CAN frames (plus clock ticks) to one battery driver's
// handle_incoming_can_frame()/transmit_can()/update_values(), built from the host unit-test
// emulation layer (test/emul). The first input byte picks the driver; see can_stream.h. The second
// byte's low 2 bits pick the chemistry, bit 2 asks for a second battery and bit 3 for a third (kept
// only for drivers that support them); a frame's target (op & 3) says which battery receives it.
#include <stddef.h>
#include <stdint.h>

#include "can_stream.h"
#include "world_reset.h"
#include "src/battery/BATTERIES.h"
#include "src/battery/CanBattery.h"
#include "src/charger/CHARGERS.h"
#include "src/datalayer/battery_aggregate.h"
#include "src/datalayer/datalayer.h"
#include "src/datalayer/datalayer_extended.h"
#include "src/devboard/hal/hal.h"
#include "src/devboard/utils/events.h"
#include "src/inverter/INVERTERS.h"

void set_millis64(uint64_t time);
void clear_transmitted_frames();

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  world_reset::snapshot();
  return 0;
}

static void reset_world() {
  // The objects go first (their pointers live in the snapshotted globals), then every firmware
  // global returns to its pre-first-input value: datalayer, per-driver statics, event table.
  delete battery;
  delete battery2;
  delete battery3;
  world_reset::restore();
  reset_all_events();
  init_hal();
  set_millis64(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return -1;
  }
  reset_world();

  // Same pack assumptions the upstream CAN-log tests use for custom-BMS batteries (90s NMC).
  user_selected_max_pack_voltage_dV = 378 + 10;
  user_selected_min_pack_voltage_dV = 261 - 10;
  user_selected_max_cell_voltage_mV = 4200 + 20;
  user_selected_min_cell_voltage_mV = 2900 - 20;
  static const battery_chemistry_enum chems[4] = {battery_chemistry_enum::NMC, battery_chemistry_enum::LFP,
                                                  battery_chemistry_enum::NCA, battery_chemistry_enum::Autodetect};
  user_selected_battery_chemistry = chems[data[1] & 3];
  user_selected_second_battery = (data[1] & 4) != 0;
  user_selected_triple_battery = (data[1] & 8) != 0;

  const int type = data[0];
  if (type >= (int)BatteryType::Highest || type == (int)BatteryType::None) {
    return -1;
  }
  user_selected_battery_type = (BatteryType)type;
  setup_battery();
  CanBattery* can_battery = dynamic_cast<CanBattery*>(battery);
  if (can_battery == nullptr) {
    // Not a CAN driver (or no driver for this enum value): nothing to feed frames to.
    return -1;
  }

  // battery2/battery3 stay null unless the driver supports parallel packs.
  CanBattery* const packs[3] = {can_battery, dynamic_cast<CanBattery*>(battery2),
                                dynamic_cast<CanBattery*>(battery3)};
  const bool multi = packs[1] != nullptr || packs[2] != nullptr;
  auto update_all = [&]() {
    for (CanBattery* pack : packs) {
      if (pack != nullptr) {
        pack->update_values();
      }
    }
    if (multi) {
      update_aggregate_values();
    }
  };

  CanStreamReader in{data + 2, data + size};
  CAN_frame frame;
  uint32_t tick_ms = 0;
  bool do_update = false;
  uint64_t now = 0;
  for (;;) {
    const int kind = in.next(&frame, &tick_ms, &do_update);
    if (kind < 0) {
      break;
    }
    if (kind == 0) {
      // A frame for a pack that is not there goes nowhere, as on the firmware's idle CAN bus.
      if (in.target < 3 && packs[in.target] != nullptr) {
        packs[in.target]->handle_incoming_can_frame(frame);
      }
    } else {
      now += tick_ms;
      set_millis64(now);
      for (CanBattery* pack : packs) {
        if (pack != nullptr) {
          pack->transmit_can(now);
        }
      }
      if (do_update) {
        update_all();
      }
      clear_transmitted_frames();
    }
  }
  update_all();
  return 0;
}
