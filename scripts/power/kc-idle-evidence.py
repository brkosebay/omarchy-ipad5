#!/usr/bin/env python3
"""Hash-checked offline j71t CPU idle anchors; no device connection."""
import hashlib
import pathlib
import struct

root = pathlib.Path(__file__).resolve().parents[2]
b = (root / 'build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12').read_bytes()
expected = 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
if hashlib.sha256(b).hexdigest() != expected:
    raise SystemExit('kernelcache identity mismatch')
print('sha256', expected)
p, segs = 32, []
for _ in range(struct.unpack_from('<I', b, 16)[0]):
    cmd, size = struct.unpack_from('<II', b, p)
    if cmd == 0x19:
        va, vs, off, fs = struct.unpack_from('<QQQQ', b, p + 24)
        segs.append((va, off, fs))
    p += size
for va, off, fs in segs:
    for i in range(off, off + fs - 3, 4):
        ins, = struct.unpack_from('<I', b, i)
        address = va + i - off
        if ins & ~31 == 0xd53df500:
            print(f'CYC_OVRD read {address:016x}: {ins:08x}')
        if 0xfffffff00718bb9c <= address < 0xfffffff00718bbec:
            print(f'idle anchor {address:016x}: {ins:08x}')
