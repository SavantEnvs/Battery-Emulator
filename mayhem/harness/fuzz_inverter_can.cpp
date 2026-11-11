// Feeds a stream of CAN frames (plus clock ticks) to one inverter protocol driver's
// map_can_frame_to_variable()/transmit_can()/update_values(), built from the host unit-test
// emulation layer (test/emul). The first input byte picks the protocol; see can_stream.h.
#include <stddef.h>
#include <stdint.h>

#include "can_stream.h"
#include "pack_state.h"
#include "world_reset.h"
#include "src/datalayer/datalayer.h"
#include "src/datalayer/datalayer_extended.h"
#include "src/devboard/hal/hal.h"
#include "src/devboard/utils/events.h"
#include "src/inverter/CanInverterProtocol.h"
#include "src/inverter/INVERTERS.h"

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
  // The object goes first (its pointer lives in the snapshotted globals), then every firmware
  // global returns to its pre-first-input value: datalayer, per-driver statics, event table.
  delete inverter;
  world_reset::restore();
  reset_all_events();
  init_hal();
  set_millis64(0);
  populate_battery_state();

  const int type = data[0];
  if (type >= (int)InverterProtocolType::Highest || type == (int)InverterProtocolType::None) {
    return -1;
  }
  user_selected_inverter_protocol = (InverterProtocolType)type;
  setup_inverter();
  CanInverterProtocol* can_inverter = dynamic_cast<CanInverterProtocol*>(inverter);
  if (can_inverter == nullptr) {
    // RS485/Modbus protocol (or no driver for this enum value): not a CAN decoder.
    return -1;
  }

  CanStreamReader in{data + 1, data + size};
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
      can_inverter->map_can_frame_to_variable(frame);
    } else {
      now += tick_ms;
      set_millis64(now);
      can_inverter->transmit_can(now);
      if (do_update) {
        can_inverter->update_values();
      }
      clear_transmitted_frames();
    }
  }
  can_inverter->update_values();
  return 0;
}
