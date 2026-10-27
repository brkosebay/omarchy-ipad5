#!/usr/bin/env python3
"""Offline validation of a static/merged/running stage-2/3 DTB and saved iomem."""
import argparse
import importlib.machinery
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[3]
FDT = importlib.machinery.SourceFileLoader(
    'aop_fdt', str(ROOT / 'scripts/boot/device/linux-a9-fdt-merge')).load_module()
POOL, SIZE = 0x858000000, 0x400000
POOL_PATH = '/reserved-memory/aop-afk@858000000'


def spans(raw):
    assert len(raw) % 16 == 0, 'expected two address/size cells'
    return [struct.unpack_from('>QQ', raw, i) for i in range(0, len(raw), 16)]


def overlap(base, size, start, length):
    return size and length and base < start + length and start < base + size


def check(blob, iomem=None, runtime=False):
    tree, reservations, _ = FDT.parse(blob)
    nodes = FDT.index(tree)
    assert b'apple,j71t' in tree.props['compatible'].split(b'\0')
    pool = nodes[POOL_PATH]
    firmware = nodes.get('/reserved-memory/aop-firmware@210e00000')
    if firmware is not None:
        assert spans(firmware.props['reg']) == [(0x210e00000, 0x9b000)]
        assert 'no-map' in firmware.props
        assert not {'compatible', 'reusable'} & firmware.props.keys()
        assert (0x210e00000, 0x9b000) in reservations
    assert (POOL, SIZE) in reservations, 'missing persistent FDT header reservation'
    assert spans(pool.props['reg']) == [(POOL, SIZE)]
    assert 'no-map' in pool.props
    assert not {'compatible', 'reusable'} & pool.props.keys()
    active = nodes['/soc/aop@210800000']
    assert active.props['memory-region'] == pool.props['phandle']
    for path, node in nodes.items():
        if path.startswith('/reserved-memory/') and path != POOL_PATH and 'reg' in node.props:
            for start, length in spans(node.props['reg']):
                assert not overlap(POOL, SIZE, start, length), f'pool overlaps {path}'
    for start, length in reservations:
        # An identical header reservation is harmless; any other overlap is not.
        assert (start, length) == (POOL, SIZE) or not overlap(POOL, SIZE, start, length), \
            f'pool overlaps FDT header reservation {start:#x}/{length:#x}'
    memory = [span for node in nodes.values()
              if node.props.get('device_type') == b'memory\0'
              for span in spans(node.props['reg'])]
    if runtime:
        assert any(start <= POOL and POOL + SIZE <= start + length for start, length in memory), \
            'pool outside bootloader-reported DRAM'
    if iomem is not None:
        rows = [m.groups() for m in re.finditer(
            r'^\s*([0-9a-fA-F]+)-([0-9a-fA-F]+)\s*:\s*(.+)$', iomem, re.M)]
        assert rows and any(int(end, 16) for _, end, _ in rows), 'iomem addresses hidden or absent'
        for start, end, label in rows:
            if label.strip() == 'System RAM':
                assert not overlap(POOL, SIZE, int(start, 16), int(end, 16) - int(start, 16) + 1), \
                    'pool still appears inside allocatable/linear-mapped System RAM'
        claimed = any(int(start, 16) == POOL and int(end, 16) + 1 == POOL + SIZE
                      and label.strip() == 'a9-aop-stage3-pool' for start, end, label in rows)
        print(f'iomem: no System RAM alias; stage3_pool_claimed={int(claimed)}')
    print(f'PASS {"runtime" if runtime else "static"} DT: AFK pool={POOL:#x}/{SIZE:#x}, no-map, exclusive, phandle resolved')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dtb', type=Path, required=True)
    p.add_argument('--iomem', type=Path)
    p.add_argument('--runtime', action='store_true')
    a = p.parse_args()
    check(a.dtb.read_bytes(), a.iomem.read_text() if a.iomem else None, a.runtime)


if __name__ == '__main__':
    main()
