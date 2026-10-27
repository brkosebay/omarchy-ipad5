#!/usr/bin/env python3
"""Add the RAM-backed apple,s8000-vxd stand-in node to a decompiled QEMU
virt DTS (scripts/vdec/qemu-smoke.sh).  Usage: qemu-dtb.py IN.dts OUT.dts BASE"""
import sys

src, dst, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
dts = open(src).read()
node = (f'\n\tvxd@{base:x} {{\n'
        '\t\tcompatible = "apple,s8000-vxd";\n'
        f'\t\treg = <0x00 {base:#x} 0x00 0x100000>;\n'
        # Edge, not the device's level: vxd-fake-mtx raises it by setting
        # the GIC pending bit, which an edge SPI consumes on acknowledge.
        '\t\tinterrupts = <0x00 0x64 0x01>;\n'
        '\t};\n')
end = dts.rindex('};')
open(dst, 'w').write(dts[:end] + node + dts[end:])
