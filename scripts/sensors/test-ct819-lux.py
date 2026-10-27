#!/usr/bin/env python3
"""Check the kernel's CT819 lux computation against an independent one.

scripts/sensors/test-ct819-lux.sh builds ct819-lux-host-test.c, which runs the
real apple_als_calc_lux() from drivers/iio/light/tsl2583.h on a host, and pipes
its table here. This re-derives every line from the LSCI blob with the formula
of docs/periph/ds-als-hall.md (section 2.5/2.6), written separately:

  counts   scaled to the record's reference time and gain, minus dark counts
  lux      a * ch0 - b * ch1 (Q16 coefficients, segment by ch1/ch0 against the
           record's limit), times fudge / 255, times trim / 1000, clamped to 5000
  clipped  a channel at 1024 counts per cycle (16 bits at most) reports 5000

usage: test-ct819-lux.py LSCI.bin < table
"""
import struct
import sys


def parse(path):
    blob = open(path, "rb").read()
    assert blob[1] == 2 and blob[2] == 1 and blob[3] == 8, "not an LSCI v2 blob"
    assert struct.unpack_from("<H", blob, 4)[0] == len(blob)
    assert sum(struct.unpack("<%dH" % (len(blob) // 2), blob)) & 0xFFFF == 0xFFFF
    r = blob[8:8 + 0x40]
    assert r[0] == 0 and r[3] == 0x40 and r[4] == 1
    u16 = lambda o: struct.unpack_from("<H", r, o)[0]
    s32 = lambda o: struct.unpack_from("<i", r, o)[0]
    cal = {
        "fudge": u16(8), "time_ms": u16(10), "gain": [1, 8, 16, 128].index(r[0x10]),
        "a1": s32(0x14), "b1": s32(0x18), "limit": s32(0x1C), "a2": s32(0x20),
        "b2": s32(0x24), "dark": (r[0x28], r[0x29]),
    }
    factors = [[65536, 65536]]
    for i in range(1, 4):
        ch0, ch1 = u16(0x30 + (i - 1) * 4), u16(0x30 + (i - 1) * 4 + 2)
        factors.append([(factors[-1][0] * ch0 + 128) // 256, (factors[-1][1] * ch1 + 128) // 256])
    cal["factors"] = factors
    return cal


def lux(cal, cycles, gain, ch0, ch1, lux_max=5000, trim=1000):
    if max(ch0, ch1) >= min(1024 * cycles - 1, 65535):
        return lux_max
    ref_cycles = cal["time_ms"] * 1000 // 2700
    norm = []
    for ch, raw in enumerate((ch0, ch1)):
        num = raw * ref_cycles * cal["factors"][cal["gain"]][ch]
        den = cycles * cal["factors"][gain][ch]
        norm.append((num + den // 2) // den)
    n0 = max(norm[0], cal["dark"][0]) - cal["dark"][0]
    n1 = max(norm[1], cal["dark"][1]) - cal["dark"][1]
    if n0 == 0:
        return 0
    a, b = (cal["a1"], cal["b1"]) if n1 * 65536 <= n0 * cal["limit"] else (cal["a2"], cal["b2"])
    scaled = n0 * a - n1 * b
    if scaled <= 0:
        return 0
    out = scaled * cal["fudge"] // (65536 * 255)
    out = out * trim // 1000
    return min(out, lux_max)


def main():
    cal = parse(sys.argv[1])
    checked = bad = 0
    for line in sys.stdin:
        if line.startswith("#"):
            print(line.rstrip())
            continue
        left, got = line.split("->")
        cycles, gain, ch0, ch1, lux_max, trim = (int(v) for v in left.split())
        want = lux(cal, cycles, gain, ch0, ch1, lux_max, trim)
        checked += 1
        if want != int(got):
            bad += 1
            if bad <= 10:
                print("MISMATCH cycles=%d gain=%d ch0=%d ch1=%d: kernel %d, reference %d"
                      % (cycles, gain, ch0, ch1, int(got), want))
    # What the unit's calibration says about plausible readings (reference gain
    # and time, sensor in the light at 1 count == 0.77 lux).
    ref_cycles = cal["time_ms"] * 1000 // 2700
    for c0, c1 in ((130, 25), (1300, 250), (6500, 1300), (30000, 6000)):
        print("example: %5d/%5d counts at %d ms, %dx -> %d lux" %
              (c0, c1, cal["time_ms"], [1, 8, 16, 128][cal["gain"]],
               lux(cal, ref_cycles, cal["gain"], c0, c1)))
    print("%d cases, %d mismatches" % (checked, bad))
    sys.exit(1 if bad or not checked else 0)


main()
