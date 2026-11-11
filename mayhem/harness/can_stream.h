// Shared input decoder for the CAN-stream harnesses.
//
// Input layout: byte 0 selects the driver (the BatteryType / InverterProtocolType enum value),
// the rest is a stream of ops:
//   op & 3 == 0..2 : one CAN frame. bit 2 = 29-bit ID (4 ID bytes LE) else 11-bit (2 ID bytes LE);
//                    bits 3..6 = DLC code (0..8 classic; 9..15 = CAN-FD lengths 12..64);
//                    followed by that many data bytes. op & 3 is the frame's target (0 = the first
//                    battery, 1 = the second, 2 = the third); harnesses with a single device ignore it.
//   op & 3 == 3    : clock tick. One byte t follows: advance the emulated clock by 1 + 4*t ms and
//                    call the driver's transmit; bit 2 of the op also calls update_values().
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "src/devboard/utils/types.h"

struct CanStreamReader {
  const uint8_t* p;
  const uint8_t* end;
  int target = 0;  // op & 3 of the last frame returned

  bool take(void* dst, size_t n) {
    if ((size_t)(end - p) < n) {
      return false;
    }
    memcpy(dst, p, n);
    p += n;
    return true;
  }

  // Returns 0 = frame in *f, 1 = tick (tick_ms, do_update set), -1 = out of input.
  int next(CAN_frame* f, uint32_t* tick_ms, bool* do_update) {
    uint8_t op;
    if (!take(&op, 1)) {
      return -1;
    }
    if ((op & 3) == 3) {
      uint8_t t;
      if (!take(&t, 1)) {
        return -1;
      }
      *tick_ms = 1 + 4u * t;
      *do_update = (op & 4) != 0;
      return 1;
    }
    static const uint8_t fd_len[7] = {12, 16, 20, 24, 32, 48, 64};
    const unsigned code = (op >> 3) & 0xF;
    memset(f, 0, sizeof(*f));
    target = op & 3;
    f->ext_ID = (op & 4) != 0;
    f->FD = code > 8;
    f->DLC = code <= 8 ? code : fd_len[code - 9];
    uint32_t id = 0;
    if (!take(&id, f->ext_ID ? 4 : 2)) {
      return -1;
    }
    f->ID = id & (f->ext_ID ? 0x1FFFFFFFu : 0x7FFu);
    if (!take(f->data.u8, f->DLC)) {
      return -1;
    }
    return 0;
  }
};
