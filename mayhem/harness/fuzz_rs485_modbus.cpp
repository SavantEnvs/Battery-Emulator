// Feeds bytes to the firmware's serial-bus (RS485 / Modbus RTU) drivers, built from the host unit-test
// emulation layer (test/emul): the BYD Modbus register server, the Kostal and Pylon LV RS485 inverter
// protocols and the Daly RS485 battery.
//
// Input layout: byte 0 & 3 selects the driver (0 BYD Modbus, 1 Kostal, 2 Pylon LV RS485, 3 Daly BMS), the
// rest is a stream of ops:
//   op & 3 == 0|1 : one chunk, a length byte n then n bytes.
//                   Modbus: one RTU frame, server id first. Op 0 appends a valid CRC, op 1 takes the bytes as
//                   they are, so the CRC check can be exercised too. Frames with a bad CRC are dropped as the
//                   real server drops them; a valid one goes to the register worker of the server.
//                   Others: the bytes land in the emulated Serial2 RX buffer and the driver's receive() runs
//                   until the buffer is empty. Bit 2 appends the frame terminator (Kostal 0x00, Pylon '\r').
//   op & 3 == 2|3 : clock tick, one byte t follows: advance the clock by 1+4t ms (op 2) or 250*(1+t) ms (op 3),
//                   transmit (Daly), receive() once, and with bit 2 also update_values().
//
// ModbusServerRTU::serve() and RTUutils::receive() are not driven: on the host micros() is constant and
// delay() a no-op, so their wait loops never end. The frame handling around them is a few lines (CRC check,
// worker lookup, CRC on the answer) and is repeated here against the same registered workers.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <new>
#include <vector>

#include "pack_state.h"
#include "world_reset.h"
#include "src/battery/DALY-BMS.h"
#include "src/datalayer/datalayer.h"
#include "src/devboard/hal/hal.h"
#include "src/devboard/utils/events.h"
#include "src/inverter/BYD-MODBUS.h"
#include "src/inverter/KOSTAL-RS485.h"
#include "src/inverter/PYLON-LV-RS485.h"
#include "src/lib/eModbus-eModbus/RTUutils.h"

void set_millis64(uint64_t time);

namespace {

// Reaches the server the Modbus inverter protocol keeps protected.
struct ModbusProbe : BydModbusInverter {
  ModbusMessage serve(ModbusMessage request) { return MBserver.localRequest(request); }
};

// The RS485 drivers add themselves to a receiver list in comm_rs485.cpp that has no way to remove
// them, so a new object per input would leak a list node every time. One object each is built before
// the global snapshot, and torn down and put back to its freshly built bytes for every input.
template <class T>
struct Rig {
  alignas(T) unsigned char mem[sizeof(T)];
  alignas(T) unsigned char pristine[sizeof(T)];

  T* obj() { return reinterpret_cast<T*>(mem); }
  void init() {
    new (mem) T();
    memcpy((void*)pristine, (void*)mem, sizeof(T));
  }
  void reset() {
    obj()->~T();
    memcpy((void*)mem, (void*)pristine, sizeof(T));
  }
};

Rig<KostalInverterProtocol> kostal;
Rig<PylonLV485InverterProtocol> pylon;
Rig<DalyBms> daly;

void serve_modbus_frame(ModbusProbe* probe, const uint8_t* frame, size_t len) {
  if (len < 4 || !RTUutils::validCRC(frame, (uint16_t)len)) {
    return;
  }
  ModbusMessage request(std::vector<uint8_t>(frame, frame + len - 2));
  if (request.size() < 2 || request[0] == 0) {
    return;  // broadcasts have no answer
  }
  ModbusMessage response = probe->serve(request);
  if (response.size() >= 3) {
    RTUutils::addCRC(response);
  }
}

void pump_receive(int device) {
  while (Serial2.available()) {
    switch (device) {
      case 1:
        kostal.obj()->receive();
        break;
      case 2:
        pylon.obj()->receive();
        break;
      default:
        daly.obj()->receive();
        break;
    }
  }
}

void update_values(int device, ModbusProbe* probe) {
  switch (device) {
    case 0:
      probe->update_values();
      break;
    case 1:
      kostal.obj()->update_values();
      break;
    case 2:
      pylon.obj()->update_values();
      break;
    default:
      daly.obj()->update_values();
      break;
  }
}

struct ProbeHolder {
  ModbusProbe* p = nullptr;
  ~ProbeHolder() { delete p; }
};

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  kostal.init();
  pylon.init();
  daly.init();
  world_reset::snapshot();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) {
    return -1;
  }
  world_reset::restore();
  reset_all_events();
  init_hal();
  set_millis64(0);
  populate_battery_state();
  datalayer.system.status.battery_allows_contactor_closing = true;

  const int device = data[0] & 3;
  ProbeHolder holder;
  switch (device) {
    case 0:
      holder.p = new ModbusProbe();
      break;
    case 1:
      kostal.reset();
      kostal.obj()->setup();
      break;
    case 2:
      pylon.reset();
      pylon.obj()->setup();
      break;
    default:
      daly.reset();
      daly.obj()->setup();
      break;
  }

  const uint8_t* p = data + 1;
  const uint8_t* const end = data + size;
  uint64_t now = 0;
  while (p < end) {
    const uint8_t op = *p++;
    if (op & 2) {
      if (p == end) {
        break;
      }
      const uint32_t t = *p++;
      now += (op & 1) ? 250u * (1 + t) : 1 + 4u * t;
      set_millis64(now);
      if (device == 3) {
        daly.obj()->transmit_rs485(now);
      }
      if (device != 0) {
        pump_receive(device);
      }
      if (op & 4) {
        update_values(device, holder.p);
      }
    } else {
      if (p == end) {
        break;
      }
      const size_t n = *p++;
      if ((size_t)(end - p) < n) {
        break;
      }
      if (device == 0) {
        std::vector<uint8_t> frame(p, p + n);
        if (!(op & 1)) {
          const uint16_t crc = RTUutils::calcCRC(frame.data(), (uint16_t)frame.size());
          frame.push_back(crc & 0xFF);
          frame.push_back(crc >> 8);
        }
        serve_modbus_frame(holder.p, frame.data(), frame.size());
      } else {
        Serial2.rx_buffer.append((const char*)p, n);
        if (op & 4) {
          Serial2.rx_buffer.push_back(device == 2 ? '\r' : '\0');
        }
        pump_receive(device);
      }
      p += n;
    }
    Serial2.tx_buffer.clear();
  }
  update_values(device, holder.p);
  return 0;
}
