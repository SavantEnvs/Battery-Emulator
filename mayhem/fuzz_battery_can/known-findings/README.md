The 5-minute Mayhem runs of `fuzz_battery_can` reported 8 defects (run 1), 9 (run 2), 10 (run 3), 11 (run 4)
and 6 (run 5). Run 5 reports fewer defects than run 4 because a single run only finds the sites its fuzzing
happens to reach, not because anything was fixed; the table below is the union over runs 1-5.
They are 10 distinct crash sites in 7 battery drivers (the `IMIEV-CZERO-ION:61` and `RANGE-ROVER-PHEV:57`
sites each have two defects in a run). All are UBSan halts on numeric conversions in upstream's
`update_values()` / `handle_incoming_can_frame()` and in a helper they call:
7 float-cast-overflow sites (a float or double is converted to an integer type that cannot hold it) and
3 signed int overflow sites. None is harness misuse: every first application frame is in `Software/src/battery/`.

Sites by the run that first reported them:

| Run | New sites |
|---|---|
| 1 | `VOLVO-SPA-HYBRID:19`, `SAMSUNG-SDI-LV:34`, `SIMPBMS:65`, `SIMPBMS:66`, `IMIEV-CZERO-ION:61`, `RANGE-ROVER-PHEV:57` |
| 2 | `NISSAN-LEAF:1666` |
| 3 | `THINK-BATTERY:17` (and `:19`, same pattern) |
| 4 | `SAMSUNG-SDI-LV:36` |
| 5 | `THINK-BATTERY:112` (and `:113`, same pattern) |

The stamp's `defects` count is the latest run only (run 5: 6 defects on 5 of these sites).

Each `.bin` here is the smallest crashing input seen for its site (from the Mayhem run, or from a local
run of the same binary). Replay one from a fresh process:

    /mayhem/fuzz_battery_can-standalone mayhem/fuzz_battery_can/known-findings/<file>.bin

The files live here, not in `testsuite/`, because seeds are replayed on every run and must not crash.

| Driver (battery type) | Site | Root cause | Impact on ESP32 | Reproducer |
|---|---|---|---|---|
| `IMIEV-CZERO-ION` (14) | `IMIEV-CZERO-ION-BATTERY.cpp:61` | `(uint16_t)(cell_voltages[i] * 1000)`: a float cell voltage above 65.535 V (decoded from CAN) does not fit `uint16_t` | reported cell voltage wraps to a small number | `imiev-czero-cell-voltage-uint16-cast.bin` |
| `NISSAN-LEAF-BATTERY` (21) | `NISSAN-LEAF-BATTERY.cpp:1666` (`Temp_fromRAW_to_F`) | `static_cast<uint16_t>(1620 - temperature * 1.81)` for `temperature >= 589`: the raw sensor value is a 16-bit field from the CAN reply (`battery_temp_raw_1..4`), and above about 895 the double is negative, which does not fit `uint16_t` | the polled min/max and `temperature1..4` Fahrenheit-to-Celsius conversions get a wrapped value for an out-of-range raw reading | `nissan-leaf-temp-raw-to-f-uint16-cast.bin` |
| `RANGE-ROVER-PHEV` (25) | `RANGE-ROVER-PHEV-BATTERY.cpp:57` | `current_dA = (CurrentExt * 0.025) - 209715` stored into `int16_t`; `CurrentExt` starts at 209715, so the result is -204472 before any frame arrives | current is wrong (wrapped) until the first frame, not merely noisy | `range-rover-phev-current-dA-overflow.bin` |
| `SAMSUNG-SDI-LV` (38) | `SAMSUNG-SDI-LV-BATTERY.cpp:34` | `system_voltage * charge_current_limit`: two `uint16_t` are promoted to `int` and overflow (signed UB) when the product exceeds 2^31 | `max_charge_power_W` is garbage | `samsung-sdi-lv-max-power-int-overflow.bin` |
| `SAMSUNG-SDI-LV` (38) | `SAMSUNG-SDI-LV-BATTERY.cpp:36` | the same product with `discharge_current_limit` (frame 0x502 bytes 4-5) for `max_discharge_power_W`. It sits on the next line, so the `:34` crash halts first whenever both limits are large; this reproducer keeps the charge limit at 0 to reach `:36` | `max_discharge_power_W` is garbage | `samsung-sdi-lv-discharge-power-int-overflow.bin` |
| `SIMPBMS` (31) | `SIMPBMS-BATTERY.cpp:65` | `celltemperature_max_dC = (raw - 273.15) * 10` converts a double to `int16_t`; raw is a 16-bit CAN field, so the result reaches 6.5e5 | max cell temperature wraps | `simpbms-cell-temp-max-int16-cast.bin` |
| `SIMPBMS` (31) | `SIMPBMS-BATTERY.cpp:66` | same expression for `celltemperature_min_dC` | min cell temperature wraps | `simpbms-cell-temp-min-int16-cast.bin` |
| `THINK-BATTERY` (`ThinkCity`, 47) | `THINK-BATTERY.cpp:17` (and the same pattern at `:19`) | `(sys_voltage * sys_currentMaxCharge) / 100`: both are `uint16_t` (`THINK-BATTERY.h`), filled from CAN frames 0x301 (bytes 2-3) and 0x303 (bytes 2-3), so they promote to `int` and `65535 * 65535` overflows (signed UB). Line 19 does the same with `sys_currentMaxDischarge` (frame 0x302 bytes 6-7) for `max_discharge_power_W` | `max_charge_power_W` and `max_discharge_power_W` are garbage for a large voltage or current | `think-city-max-power-int-overflow.bin` |
| `THINK-BATTERY` (`ThinkCity`, 47) | `THINK-BATTERY.cpp:112` (and the same pattern at `:113`) | `max_cellvoltage = ((u8[0] << 8 | u8[1]) * 2.44141)` converts a double of up to about 160000 into the `uint16_t max_cellvoltage` (`THINK-BATTERY.h`); the operands are bytes 0-1 of CAN frame 0x610. Line 113 does the same with bytes 2-3 for `min_cellvoltage`; the `:112` crash halts first whenever both are large, so this reproducer keeps bytes 2-3 at 0 | `max_cellvoltage` and `min_cellvoltage` wrap to a small number for a raw value above 26843 (about 65.5 V) | `think-city-cell-voltage-uint16-cast.bin` |
| `VOLVO-SPA-HYBRID` (36) | `VOLVO-SPA-HYBRID-BATTERY.cpp:19` | `BECMBatteryCurrent = BATT_I`: `BATT_I` is a float that is negative while charging, stored into `uint16_t` | the extended-data `BECMBatteryCurrent` is wrong while charging | `volvo-spa-hybrid-becm-current-uint16-cast.bin` |

Suggested fixes are one-liners (clamp to the destination range, or widen the destination / use `int32_t`
for the product); upstream's drivers are not patched here.

## Reproduction from a fresh process

Before the per-input reset (`harness/world_reset.h`) was added, a crash could in principle depend on
which driver had run earlier in the same process. All 29 distinct crashing inputs the first Mayhem run
saved (covering all 8 defects) were replayed one by one through `/mayhem/fuzz_battery_can-standalone`,
each in its own process on the pre-reset image, and every one crashed at its original site. On the image
with the reset they still do, and they crash identically when replayed after all 70 seeds in one process.

Run 2 (on the image with the reset) saved 34 crashing inputs covering all 9 defects. Each was replayed in
its own fresh `/mayhem/fuzz_battery_can-standalone` process and crashed at the site Mayhem reported. The
Nissan Leaf input (defect #3601751) is a single crasher from that run, reduced from 1219 to 204 bytes by
delta debugging that required the `NISSAN-LEAF-BATTERY.cpp:1666` site to be kept.

Run 3 (same image) saved 37 crashing inputs covering all 10 defects. The new site, `ThinkBattery::update_values()`
(defect #3602250), has one crasher, 519 bytes. It replays from a fresh `/mayhem/fuzz_battery_can-standalone`
process and stops at `THINK-BATTERY.cpp:17:62` (`signed integer overflow: 65535 * 65535`). `-minimize_crash`
drifted to other sites last round, so `think-city-max-power-int-overflow.bin` is not a libFuzzer-minimized
file: it is 16 bytes written from the driver's CAN decoder (type 47, chemistry byte, frame 0x301 with
voltage 0xFFFF, frame 0x303 with charge current 0xFFFF, then the final `update_values()`), and checked
against the site: it stops at `THINK-BATTERY.cpp:17:62`, and deleting any single byte loses that crash.

Run 4 (same image, 11 defects) added one defect, #3602623, at `SAMSUNG-SDI-LV-BATTERY.cpp:36:67`
(`signed integer overflow: 65535 * 65535`). It needs no harness change: the harness already delivers frame
0x502, and the `:34` and `:36` expressions are independent lines of the same `update_values()`.
`samsung-sdi-lv-discharge-power-int-overflow.bin` is 24 bytes written from the driver's CAN decoder (type 38,
chemistry byte, frame 0x500 with voltage 0xFFFF, frame 0x502 with charge limit 0 and discharge limit 0xFFFF,
then the final `update_values()`); it replays from a fresh `/mayhem/fuzz_battery_can-standalone` process and
stops at `SAMSUNG-SDI-LV-BATTERY.cpp:36:67`.

Run 5 (same image, 6 defects) added one site, #3602921, `ThinkBattery::handle_incoming_can_frame()` at
`THINK-BATTERY.cpp:112:25` (`159998 is outside the range of representable values of type 'unsigned short'`).
It needs no harness change: the harness already delivers frame 0x610. Mayhem saved 17 crashing inputs for
run 5; the Think one (94 bytes) replays from a fresh `/mayhem/fuzz_battery_can-standalone` process and stops
at `THINK-BATTERY.cpp:112:25`. `think-city-cell-voltage-uint16-cast.bin` is not libFuzzer-minimized (that has
drifted to other sites before): it is 9 bytes written from the driver's CAN decoder (type 47, chemistry byte,
frame 0x610 with DLC 4 and bytes 0-1 = 0xFFFF), and checked against the site: it stops at
`THINK-BATTERY.cpp:112:25`, and deleting any single byte loses that crash. With bytes 2-3 = 0xFFFF and
bytes 0-1 = 0 instead, the same frame stops at `:113:25`.

## Second and third battery

`fuzz_battery_can` also drives a second and a third battery. Bits 2 and 3 of the byte after the battery type
set `user_selected_second_battery` / `user_selected_triple_battery` (the low two bits still pick the
chemistry); the target of a frame op (`op & 3`, see `harness/can_stream.h`) routes it to battery 1, 2 or 3,
and ticks transmit on all of them, so the aggregate (`update_aggregate_values()`) is exercised too. Only the
drivers that `setup_battery()` can build a second or third time get one (Nissan LEAF, for example); for the rest
the bits change nothing. `testsuite/multi_pack_nissan_leaf` is a three-pack seed.
