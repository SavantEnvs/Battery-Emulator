// Per-input reset of the firmware's mutable globals. Upstream's drivers keep state in file-scope
// statics (frame counters, last-seen values, ...) that no reset function covers, so without this
// an input's behavior would depend on which drivers ran before it. world_reset.ld gathers every
// such variable of the firmware library into the ranges [__fw_data_start, __fw_data_end) and
// [__fw_bss_start, __fw_bss_end); the image taken before the first input is written back before
// each one.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include <Arduino.h>
#include <HardwareSerial.h>

#include "src/communication/can/comm_can.h"
#include "src/devboard/hal/hal.h"

extern "C" {
extern char __fw_data_start[], __fw_data_end[];
extern char __fw_bss_start[], __fw_bss_end[];
}

// Defined in test/emul/can.cpp.
extern std::vector<CAN_frame> g_emul_transmitted_frames;
extern std::vector<CAN_Interface> g_emul_transmitted_interfaces;
// Defined in test/emul/Arduino.cpp.
extern std::vector<PinWrite> g_emul_pin_writes;
extern std::vector<DutyWrite> g_emul_duty_writes;
extern std::vector<ToneWrite> g_emul_tone_writes;

namespace world_reset {

struct Region {
  char* begin;
  char* end;
  uint64_t* saved;
};

static Region regions[2];

// Plain word loops, not memcpy: ASan intercepts memcpy and would flag the redzones between globals.
__attribute__((no_sanitize("address", "undefined"), noinline)) static void copy_words(volatile uint64_t* dst,
                                                                                       const volatile uint64_t* src,
                                                                                       size_t words) {
  for (size_t i = 0; i < words; i++) {
    dst[i] = src[i];
  }
}

static inline size_t words_of(const Region& r) {
  return (size_t)(r.end - r.begin) / sizeof(uint64_t);
}

// Call once, after static constructors and before the first input.
static inline void snapshot() {
  regions[0] = {__fw_data_start, __fw_data_end, nullptr};
  regions[1] = {__fw_bss_start, __fw_bss_end, nullptr};
  for (Region& r : regions) {
    r.begin = (char*)(((uintptr_t)r.begin + 7) & ~(uintptr_t)7);
    r.end = (char*)((uintptr_t)r.end & ~(uintptr_t)7);
    r.saved = (uint64_t*)malloc(words_of(r) * sizeof(uint64_t));
    copy_words(r.saved, (uint64_t*)r.begin, words_of(r));
  }
}

// Restoring the image puts back the pre-input (empty) pointers, so heap blocks owned by the globals
// are freed first or they would be orphaned on every input.
static inline void restore() {
  delete esp32hal;
  std::vector<CAN_frame>().swap(g_emul_transmitted_frames);
  std::vector<CAN_Interface>().swap(g_emul_transmitted_interfaces);
  std::vector<PinWrite>().swap(g_emul_pin_writes);
  std::vector<DutyWrite>().swap(g_emul_duty_writes);
  std::vector<ToneWrite>().swap(g_emul_tone_writes);
  for (HardwareSerial* serial : {&Serial, &Serial1, &Serial2}) {
    std::string().swap(serial->rx_buffer);
    std::string().swap(serial->tx_buffer);
  }
  for (Region& r : regions) {
    copy_words((uint64_t*)r.begin, r.saved, words_of(r));
  }
}

}  // namespace world_reset
