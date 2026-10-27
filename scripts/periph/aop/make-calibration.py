#!/usr/bin/env python3
"""Host-only: build the per-device accel calibration firmware file.

Reads the accel node of a saved IODeviceTree text dump (default: the private
live dump of this iPad) and writes the blobs iOS sends to the AOP, in the iOS
table order (fffffff006d5e7f8), as apple/aop/accel-calibration.bin:

    "A9AC" | u32 version=1 | u32 count | count x {u32 key, u32 len, bytes}

Only blobs present on the device are written; absent ones are skipped, never
zero-filled. The output is per-device factory calibration: it stays under the
ignored build/ tree and on this device's root filesystem. Never put it in a
public DTS or repository, and never write it back to syscfg.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_DUMP = ROOT / 'build/firmware/private/IODeviceTree.txt'
DEFAULT_OUT = ROOT / 'build/periph/aop-r4/firmware/apple/aop/accel-calibration.bin'

# (ADT property, AOP key) for the unflagged accel entries, iOS order.
TABLE = (
    ('low-temp-accel-offset', 0x1a),
    ('accel-sensitivity-calibration', 0x0b),
    ('accel-range-sensitivity-cal', 0x27),
    ('accel-orientation', 0x18),
    ('accel-interrupt-calibration', 0x19),
)
MAGIC = b'A9AC'
MAX_BLOB = 64


def accel_properties(text):
    """Return {name: bytes} for the first `accel` AppleSPUHIDInterface node."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if re.search(r'\+-o accel\s+<class AppleSPUHIDInterface', line):
            break
    else:
        raise ValueError('accel AppleSPUHIDInterface node not found')
    props = {}
    for line in lines[i + 1:]:
        if '+-o ' in line:
            break
        m = re.search(r'"([^"]+)" = <([0-9a-fA-F]*)>', line)
        if m:
            props[m.group(1)] = bytes.fromhex(m.group(2))
    return props


def build(props):
    entries = []
    for name, key in TABLE:
        blob = props.get(name)
        if blob is None:
            continue
        if not blob or len(blob) > MAX_BLOB:
            raise ValueError(f'{name}: unexpected size {len(blob)}')
        entries.append((name, key, blob))
    if not entries:
        raise ValueError('no accel calibration blobs present')
    out = MAGIC + struct.pack('<II', 1, len(entries))
    for _, key, blob in entries:
        out += struct.pack('<II', key, len(blob)) + blob
    return out, entries


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dump', type=Path, default=DEFAULT_DUMP)
    ap.add_argument('--output', type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()
    data, entries = build(accel_properties(args.dump.read_text(errors='replace')))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    args.output.chmod(0o600)
    for name, key, blob in entries:
        print(f'key 0x{key:02x} {name}: {len(blob)} bytes')
    print(f'{args.output} {len(data)} bytes sha256 {hashlib.sha256(data).hexdigest()}')
    print('Per-device data: stage only to this iPad\'s /lib/firmware/apple/aop/.')


if __name__ == '__main__':
    main()
