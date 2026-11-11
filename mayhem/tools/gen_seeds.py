#!/usr/bin/env python3
"""Regenerate the starter seeds (format: harness/can_stream.h).

Battery seeds: one clock-tick-only input per battery type that has a CAN driver, plus the CAN logs
upstream ships under test/can_log_based/can_logs/ converted to the harness stream.
Inverter seeds: per protocol, ticks plus one zero-payload frame per CAN id the driver switches on.
RS485/Modbus seeds (harness/fuzz_rs485_modbus.cpp): valid Modbus RTU requests for the BYD register server and
well-formed Kostal, Pylon LV and Daly frames, so a run starts past the length/CRC/header checks.
Peripheral seeds (harness/fuzz_can_peripherals.cpp): the charger and shunt CAN ids, plus ticks.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
FD_LEN = [12, 16, 20, 24, 32, 48, 64]

BATTERY_TYPES = [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 47, 48, 49, 50, 51, 52,
                 53, 54, 55, 56, 57, 58, 59]
# Tick-only input already crashes these (see fuzz_battery_can/known-findings); seeds must not crash.
CRASHING_TICK_ONLY = {25}
INVERTERS = {1: "AFORE-CAN", 2: "BYD-CAN", 4: "FERROAMP-CAN", 5: "FOXESS-CAN", 6: "GROWATT-HV-CAN",
             7: "GROWATT-LV-CAN", 8: "GROWATT-WIT-CAN", 10: "PYLON-CAN", 11: "PYLON-LV-CAN",
             12: "SCHNEIDER-CAN", 14: "SMA-BYD-H-CAN", 15: "SMA-LV-CAN", 16: "SMA-BYD-HVS-CAN", 17: "SOFAR-CAN",
             18: "SOLAX-CAN", 19: "SOLXPOW-CAN", 20: "SOL-ARK-LV-CAN", 21: "SUNGROW-CAN", 22: "VCU-CAN",
             24: "SMA-SBS-BYD-CAN", 25: "FOXESS-EP-CAN"}


def tick(ms, update=False):
    t = max(0, min(255, (ms - 1) // 4))
    return bytes([3 | (4 if update else 0), t])


def frame(fid, data, ext=None):
    if ext is None:
        ext = fid > 0x7FF
    n = len(data)
    if n <= 8:
        code = n
    else:
        fit = [i for i, l in enumerate(FD_LEN) if l <= n]
        if not fit:
            return b""
        code = 9 + fit[-1]
        data = data[:FD_LEN[fit[-1]]]
    op = (code << 3) | (4 if ext else 0)
    idb = (fid & 0x1FFFFFFF).to_bytes(4, "little") if ext else (fid & 0x7FF).to_bytes(2, "little")
    return bytes([op]) + idb + bytes(data)


def modbus_crc(b):
    crc = 0xFFFF
    for x in b:
        crc ^= x
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def chunk(payload, op=0):
    return bytes([op, len(payload)]) + bytes(payload)


def rtu(payload):
    """A Modbus RTU frame with its own CRC (op 1: the harness does not add one)."""
    return chunk(bytes(payload) + modbus_crc(bytes(payload)).to_bytes(2, "little"), 1)


def u16(*v):
    return b"".join(x.to_bytes(2, "big") for x in v)


def big_tick(quarter_s, update=False):
    return bytes([3 | (4 if update else 0), quarter_s - 1])


def kostal_frame(plain):
    """plain = header + payload, without pointer byte, CRC and end byte; returns the stuffed wire frame."""
    body = bytes([0]) + bytes(plain)
    crc = (-sum(body[1:])) & 0xFF
    assert crc != 0
    f = bytearray(body + bytes([crc, 0]))
    last = 0
    for i in range(len(f)):
        if f[i] == 0 and i > 0:
            f[last] = i - last
            last = i
    return bytes(f)


def rtick(ms, update=False):
    return bytes([2 | (4 if update else 0), max(0, min(255, (ms - 1) // 4))])


def pylon_frame(cid2):
    return ("~200246" + cid2 + "E00200FD37\r").encode()


def daly_frame(cmd, data):
    f = bytes([0xA5, 0x01, cmd, 8]) + bytes(data)
    return f + bytes([sum(f) & 0xFF])


def write(d, name, blob):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(blob)


def main():
    bdir = os.path.join(HERE, "..", "fuzz_battery_can", "testsuite")
    idir = os.path.join(HERE, "..", "fuzz_inverter_can", "testsuite")
    for t in BATTERY_TYPES:
        if t in CRASHING_TICK_ONLY:
            continue
        write(bdir, f"tick_type{t:02d}", bytes([t, 0]) + b"".join(tick(10 + 50 * i, i % 2 == 1) for i in range(40)))

    logdir = os.path.join(ROOT, "test", "can_log_based", "can_logs")
    line = re.compile(r"\((\d+\.\d+)\)\s+\S+\s+([0-9A-Fa-f]+)\s+\[(\d+)\]\s*((?:[0-9A-Fa-f]{2}\s*)*)")
    for fn in sorted(os.listdir(logdir)):
        m = re.match(r"(\d+)_", fn)
        if not m:
            continue
        t = int(m.group(1))
        blob = bytes([t, 0])
        last = None
        n = 0
        for ln in open(os.path.join(logdir, fn), errors="replace"):
            mm = line.match(ln)
            if not mm:
                continue
            ts, fid, dlc, hexdata = float(mm.group(1)), int(mm.group(2), 16), int(mm.group(3)), mm.group(4).split()
            data = bytes(int(x, 16) for x in hexdata)[:dlc]
            if last is not None and ts - last >= 0.005:
                blob += tick(int((ts - last) * 1000), True)
            last = ts
            blob += frame(fid, data)
            n += 1
            if n >= 300:
                break
        write(bdir, "log_" + os.path.splitext(fn)[0], blob)

    for t, stem in INVERTERS.items():
        src = open(os.path.join(ROOT, "Software", "src", "inverter", stem + ".cpp"), errors="replace").read()
        ids = []
        for x in re.findall(r"case\s+(0x[0-9A-Fa-f]+)\s*:", src):
            v = int(x, 16)
            if v not in ids:
                ids.append(v)
        blob = bytes([t])
        blob += tick(10)
        for fid in ids[:24]:
            blob += frame(fid, bytes(8))
        for i in range(30):
            blob += tick(100 + 100 * i, i % 3 == 0)
        write(idir, f"proto{t:02d}_{stem}", blob)

    # Three Nissan LEAF packs (battery byte 1: bit 2 = second pack, bit 3 = third pack): the same frames
    # addressed to each of them through the frame target (op & 3), then ticks with updates.
    multi = bytes([21, 4 | 8])
    for target in (0, 1, 2):
        for fid in (0x1DB, 0x1DC, 0x55B, 0x5BC, 0x5C0, 0x59E, 0x1F2, 0x1D5):
            f = frame(fid, bytes([0x12, 0x34, 0x56, 0x78, 0x10, 0x20, 0x30, 0x40]))
            multi += bytes([f[0] | target]) + f[1:]
    multi += b"".join(tick(10 + 50 * i, i % 2 == 1) for i in range(40))
    write(bdir, "multi_pack_nissan_leaf", multi)

    rdir = os.path.join(HERE, "..", "fuzz_rs485_modbus", "testsuite")
    # BYD Modbus (server id 21): register writes, then reads, then the control block and an update.
    byd = bytes([0])
    byd += rtu([21, 0x06, 0x01, 0x91, 0x00, 0x05])  # FC06 captured shape: write 5 to register 401
    byd += chunk([21, 0x10, 0x01, 0x90, 0x00, 0x08, 0x10] + list(u16(0, 1, 60, 0, 0, 0, 0, 0)))  # FC16 400..407
    byd += chunk([21, 0x03, 0x00, 0xC8, 0x00, 0x0D])  # FC03 200..212
    byd += chunk([21, 0x03, 0x01, 0x2C, 0x00, 0x18])  # FC03 300..323
    byd += chunk([21, 0x17, 0x00, 0x64, 0x00, 0x10, 0x01, 0x90, 0x00, 0x02, 0x04] + list(u16(0, 1)))  # FC23
    byd += chunk([21, 0x03, 0x75, 0x30, 0x00, 0x01])  # FC03 past the register limit: error answer
    byd += chunk([21, 0x04, 0x00, 0x00, 0x00, 0x01])  # unserved function code
    byd += chunk([22, 0x03, 0x00, 0x00, 0x00, 0x01])  # another server id
    for i in range(6):
        byd += bytes([2 | 4, 40])
    write(rdir, "byd_modbus_fc03_fc06_fc16_fc23", byd)

    hdr_b = [0x62, 0xFF, 0x02, 0xFF, 0x29]
    hdr_c = [0x63, 0xFF, 0x02, 0xFF, 0x29]
    kos = bytes([1]) + big_tick(4, True) + big_tick(4, True) + big_tick(4, True)  # past the 2 s RX_allow delay
    kos += chunk(kostal_frame(hdr_b + [0x4A, 0x08]))  # battery info request
    kos += chunk(kostal_frame(hdr_b + [0x4A, 0x04]))  # cyclic data request
    kos += big_tick(4, True)
    kos += chunk(kostal_frame(hdr_b + [0x53, 0x03]))  # status request
    kos += chunk(kostal_frame(hdr_c + [0x47, 0x01]))  # set time
    kos += chunk(kostal_frame(hdr_c + [0x5E, 0x00]))  # allow contactor closing
    kos += chunk(kostal_frame(hdr_c + [0x5E, 0x04]))  # contactor test
    kos += chunk(kostal_frame(hdr_c + [0x5E, 0x07]))
    kos += chunk(kostal_frame(hdr_b + [0x50, 0x01]))  # reverse polarity
    kos += big_tick(20, True) + big_tick(20, True)
    write(rdir, "kostal_info_cyclic_status_set_state", kos)

    pyl = bytes([2]) + rtick(10)
    for cid2 in ("42", "61", "44", "62", "92", "63", "FF"):
        pyl += chunk(pylon_frame(cid2)) + rtick(100, True)
    pyl += chunk(b"~20", 4) + chunk(b"X", 0)  # a short frame cut off by the terminator, then noise
    write(rdir, "pylon_lv_485_cid2_requests", pyl)

    dal = bytes([3]) + rtick(70)
    dal += chunk(daly_frame(0x90, u16(3840, 0, 30150, 650)))  # 384.0 V, +15.0 A, 65.0 % SoC
    dal += chunk(daly_frame(0x91, u16(3700, 0, 3655) + bytes(2)))
    dal += chunk(daly_frame(0x92, [60, 1, 55, 2, 0, 0, 0, 0]))
    dal += chunk(daly_frame(0x93, [0, 0, 0, 0, 0, 1, 0x86, 0xA0]))
    dal += chunk(daly_frame(0x94, [96, 4, 0, 0, 0, 0, 0, 0]))
    for fi in range(1, 33):
        dal += chunk(daly_frame(0x95, [fi] + list(u16(3700 + fi, 3690, 3680)) + [0]))
    dal += chunk(daly_frame(0x96, [1] + [0] * 7)) + chunk(daly_frame(0x97, [0] * 8))
    dal += chunk(daly_frame(0x98, [0] * 8)) + rtick(100, True) + rtick(100, True)
    dal += chunk(daly_frame(0x98, [0, 0, 0, 0, 0, 0, 0, 1])) + rtick(100, True)
    dal += chunk(bytes([0xA5, 0x01, 0x90]), 0) + chunk(bytes([0x00]) * 5, 0)  # truncated frame, then noise
    write(rdir, "daly_bms_status_frames", dal)

    cdir = os.path.join(HERE, "..", "fuzz_can_peripherals", "testsuite")

    def tgt(blob, target):
        """Set the frame target (op & 3) of every frame op in a frame-only blob."""
        return bytes([blob[0] | target]) + blob[1:]

    leaf = bytes([1])
    for fid, data in ((0x5BC, [0] * 8), (0x679, [0x20] + [0] * 7), (0x390, [0] * 8), (0x393, [0] * 8)):
        leaf += tgt(frame(fid, bytes(data)), 0)
    leaf += b"".join(tick(10 + 10 * i, i % 2 == 1) for i in range(40))
    write(cdir, "charger_nissan_leaf", leaf)

    volt = bytes([2])
    for fid in (0x212, 0x30A, 0x266, 0x268, 0x308):
        volt += tgt(frame(fid, bytes([0x03, 0x84, 0x00, 0x12, 0x20, 0x01, 0, 0])), 0)
    volt += b"".join(tick(30 + 30 * i, i % 2 == 1) for i in range(40))
    write(cdir, "charger_chevy_volt", volt)

    sbox = bytes([4 | 1])
    for fid in (0x200, 0x210, 0x220):
        sbox += tgt(frame(fid, bytes([0x10, 0x27, 0, 0, 0xE8, 0x03, 0, 0])), 1)
    sbox += b"".join(tick(100 + 100 * i, i % 2 == 1) for i in range(60))
    write(cdir, "shunt_bmw_sbox", sbox)


if __name__ == "__main__":
    main()
