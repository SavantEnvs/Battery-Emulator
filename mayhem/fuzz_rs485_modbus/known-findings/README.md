# Known findings in `fuzz_rs485_modbus`

`fuzz_rs485_modbus` drives the serial-bus drivers: the BYD Modbus register server (FC03/FC06/FC16/FC23 and
RTU framing/CRC), the Kostal and Pylon LV RS485 inverter protocols and the Daly RS485 battery. Input layout:
`harness/fuzz_rs485_modbus.cpp`.

| Driver | Site | Root cause | Impact on ESP32 | Reproducer |
|---|---|---|---|---|
| BYD Modbus (`ModbusInverterProtocol`) | `ModbusInverterProtocol.cpp:60` (`FC03`) | `for (uint8_t i = 0; i < words; ++i)` with `words` a `uint16_t` up to 29999 (the address check only bounds `addr + words`): for `words >= 256` the 8-bit counter wraps and the loop never ends, appending to `response` forever | a single valid read request (server id 21, FC03, 256 or more registers) on the RS485 bus hangs the Modbus server task and grows its heap until the task watchdog or memory runs out | `byd-modbus-fc03-words-over-255-hang.bin` |

The reproducer is the 9-byte stream: driver 0, one RTU frame `15 03 00 00 01 00` (server 21, FC03, address 0,
256 words; the harness appends the CRC). It does not finish: run the standalone under `timeout`.

    timeout 20 /mayhem/fuzz_rs485_modbus-standalone mayhem/fuzz_rs485_modbus/known-findings/byd-modbus-fc03-words-over-255-hang.bin

libFuzzer reports it as a timeout, with `ModbusInverterProtocol::FC03` on the stack. The fix is a one-liner
(`uint16_t i`, or reject `words > 125` as FC23 and the Modbus spec do). Upstream is not patched here. The file
lives here, not in `testsuite/`, because seeds are replayed on every run and must not hang.

`ModbusServerRTU::serve()` and `RTUutils::receive()` are not driven by the harness: on the host `micros()` is
constant and `delay()` a no-op, so their wait loops never end. The harness repeats the few lines around the
worker call (CRC check, worker lookup through `ModbusServer::localRequest`, CRC on the answer).
