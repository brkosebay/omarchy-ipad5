#!/usr/bin/env python3
"""Extract the personalized SEP IMG4 from the device's boot-firmware area.

The iPad's nvme0n2 (8 MiB) is the NOR/all_flash container: DER-wrapped IMG4s
written by the restore, personalized for this device. Find the one whose IM4P
type is "sepi" and write it out.
"""
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "/dev/nvme0n2"
DST = sys.argv[2] if len(sys.argv) > 2 else "/tmp/sep-firmware.img4"

data = open(SRC, "rb").read()
found = []
i = 0
while True:
    j = data.find(b"IMG4", i)
    if j < 0:
        break
    # DER: 30 <len> 16 04 "IMG4"  ->  string starts 7 bytes in (5-byte long form)
    start = j - 7
    if (start >= 0 and data[start] == 0x30 and data[start + 1] in (0x81, 0x82, 0x83)
            and data[j - 2] == 0x16 and data[j - 1] == 0x04):
        n = data[start + 1]
        if n == 0x83:
            ln = int.from_bytes(data[start + 2:start + 5], "big"); hdr = 5
        elif n == 0x82:
            ln = int.from_bytes(data[start + 2:start + 4], "big"); hdr = 4
        else:
            ln = data[start + 2]; hdr = 3
        end = start + hdr + ln
        blob = data[start:end]
        k = blob.find(b"\x16\x04sepi")
        typ = "sepi" if k >= 0 else "?"
        found.append((start, len(blob), typ, blob.find(b"IM4R") >= 0))
    i = j + 4

print("IMG4 candidates (offset, size, sepi?, has IM4R):")
for f in found:
    print("  %#x  %8d  %s  %s" % f)

sepi = [f for f in found if f[2] == "sepi"]
if not sepi:
    sys.exit("no sepi IMG4 found")
off, size, _, _ = sepi[0]
blob = data[off:off + size]
open(DST, "wb").write(blob)
print("wrote %s (%d bytes) from %#x" % (DST, len(blob), off))
