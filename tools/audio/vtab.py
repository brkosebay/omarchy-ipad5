#!/usr/bin/env python3
"""Print a kext vtable: tools/audio/vtab.py KEXTNAME METHOD_VA OFFSET_OF_METHOD [first last]
Finds METHOD_VA in the kext's __DATA_CONST.__const, takes vtable base =
slot - OFFSET, prints slots [first,last) (byte offsets) that point into __text."""
import struct, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2] / "build/audio/kexts"
kext, meth, off = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
first = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
last = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0x800
secs = {}
import subprocess
out = subprocess.run([sys.executable, "-I", str(Path(__file__).parent / "kcmap.py"), "sections",
                      {"AppleAE2Audio": "com.apple.driver.AppleAE2Audio"}.get(kext, kext)],
                     capture_output=True, text=True).stdout
for l in out.splitlines():
    n, va, sz = l.split("\t"); secs[n] = (int(va, 16), int(sz, 16))
cva, csz = secs["__DATA_CONST.__const"]
tva, tsz = secs["__TEXT_EXEC.__text"]
d = (root / f"{kext.split('.')[-1]}.__DATA_CONST.__const.bin").read_bytes()
w = struct.unpack("<%dQ" % (len(d) // 8), d)
for i, x in enumerate(w):
    if x == meth:
        base = cva + i * 8 - off
        print(f"vtable base {base:#x} (slot {cva + i*8:#x})")
        for o in range(first, last, 8):
            j = (base - cva + o) // 8
            if 0 <= j < len(w):
                v = w[j]
                tag = "*" if tva <= v < tva + tsz else " "
                print(f"  +{o:#05x} {v:#x} {tag}")
