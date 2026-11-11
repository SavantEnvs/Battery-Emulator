// Feeds a stream of CAN frames (plus clock ticks) to the CAN charger drivers (Nissan LEAF PDM, Chevy
// Volt) and the CAN shunt driver (BMW SBOX), built from the host unit-test emulation layer (test/emul).
//
// Input byte 0: bits 0-1 pick the charger (1 Nissan LEAF, 2 Chevy Volt, else none), bit 2 adds the BMW SBOX
// shunt. The rest is the shared op stream of can_stream.h; the frame target selects who receives it:
// 0 the charger, 1 the shunt, 2 both. A tick calls transmit on both, and with the update bit reads the
// charger's derived outputs (power, current, voltage, efficiency).
#include <stddef.h>
#include <stdint.h>

#include "can_stream.h"
#include "pack_state.h"
#include "world_reset.h"
#include "src/battery/BATTERIES.h"
#include "src/charger/CHARGERS.h"
#include "src/datalayer/datalayer.h"
#include "src/devboard/hal/hal.h"
#include "src/devboard/utils/events.h"
#include "src/shunt/BMW-SBOX.h"
#include "src/shunt/Shunt.h"

void set_millis64(uint64_t time);
void clear_transmitted_frames();

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  world_reset::snapshot();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) {
    return -1;
  }
  // The objects go first (their pointers live in the snapshotted globals), then every firmware
  // global returns to its pre-first-input value.
  delete charger;
  delete shunt;
  world_reset::restore();
  reset_all_events();
  init_hal();
  set_millis64(0);
  populate_battery_state();

  const int charger_type = data[0] & 3;
  user_selected_charger_type =
      charger_type == 1 ? ChargerType::NissanLeaf : (charger_type == 2 ? ChargerType::ChevyVolt : ChargerType::None);
  user_selected_shunt_type = (data[0] & 4) ? ShuntType::BmwSbox : ShuntType::None;
  setup_charger();
  setup_shunt();
  CanShunt* can_shunt = dynamic_cast<CanShunt*>(shunt);
  if (charger == nullptr && can_shunt == nullptr) {
    return -1;
  }

  CanStreamReader in{data + 1, data + size};
  CAN_frame frame;
  uint32_t tick_ms = 0;
  bool do_update = false;
  uint64_t now = 0;
  volatile float sink = 0;
  for (;;) {
    const int kind = in.next(&frame, &tick_ms, &do_update);
    if (kind < 0) {
      break;
    }
    if (kind == 0) {
      if (charger != nullptr && in.target != 1) {
        charger->map_can_frame_to_variable(frame);
      }
      if (can_shunt != nullptr && in.target != 0) {
        can_shunt->handle_incoming_can_frame(frame);
      }
    } else {
      now += tick_ms;
      set_millis64(now);
      if (charger != nullptr) {
        charger->transmit_can(now);
      }
      if (can_shunt != nullptr) {
        can_shunt->transmit_can(now);
      }
      if (do_update && charger != nullptr) {
        sink = sink + charger->outputPowerDC() + charger->HVDC_output_voltage() + charger->HVDC_output_current() +
                charger->LVDC_output_voltage() + charger->LVDC_output_current() + charger->AC_input_voltage() +
                charger->AC_input_current();
        if (charger->efficiencySupported()) {
          sink = sink + charger->efficiency();
        }
      }
      clear_transmitted_frames();
    }
  }
  return 0;
}
