#!/usr/bin/env python3
"""Map saved j71t PMGR ADT devices to Linux domains and direct DT consumers.

Offline only. The output is topology, not a register snapshot or probe order.
"""
import argparse
import hashlib
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[2]


def find_pmgr(value):
    if isinstance(value, dict):
        if 'pmgr' in value:
            return value['pmgr']
        for item in value.values():
            result = find_pmgr(item)
            if result is not None:
                return result
    elif isinstance(value, list):
        for item in value:
            result = find_pmgr(item)
            if result is not None:
                return result
    return None


def cells(value):
    return list(struct.unpack('>' + 'I' * (len(value)//4), value))


def snapshots(path):
    result = {}
    for line in path.read_text(errors='replace').splitlines():
        match = re.search(r'PMGR (\w+) raw before floor @([0-9a-f]+) reg=([0-9a-f]{8})', line)
        # Only main PMGR: mini/AOP offsets can alias the main register map.
        if not match or '20e000000.power-management' not in line:
            continue
        name, offset, raw = match.groups()
        value = int(raw, 16)
        result[int(offset, 16)] = {
            'label': name, 'raw': raw, 'target': value & 15,
            'actual': (value >> 4) & 15, 'minimum': (value >> 16) & 15,
            'auto_enable': bool(value & (1 << 28)),
            'reset': bool(value & (1 << 31)), 'disable': bool(value & (1 << 10))}
    if not result:
        raise ValueError(f'no main-PMGR raw snapshots in {path}; capture full dmesg with apple_pmgr.trace enabled')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dtb', type=Path,
                        default=ROOT/'build/kernel-integ/dtbs/s8003-j71t.dtb')
    parser.add_argument('--adt', type=Path, default=ROOT/'docs/adt/j71t.json')
    parser.add_argument('--cold-log', type=Path)
    parser.add_argument('--warm-log', type=Path)
    args = parser.parse_args()
    if bool(args.cold_log) != bool(args.warm_log):
        parser.error('supply both --cold-log and --warm-log')
    cold = snapshots(args.cold_log) if args.cold_log else {}
    warm = snapshots(args.warm_log) if args.warm_log else {}
    loader = importlib.machinery.SourceFileLoader(
        'a9_fdt', str(ROOT/'scripts/boot/device/linux-a9-fdt-merge'))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    fdt = importlib.util.module_from_spec(spec)
    loader.exec_module(fdt)
    blob = args.dtb.read_bytes()
    tree, _, _ = fdt.parse(blob)
    nodes = fdt.index(tree)
    adt = find_pmgr(json.loads(args.adt.read_text()))
    assert adt is not None
    # Main PMGR reg 0 only. The mini/AOP window has a different MMIO base.
    devices = {adt['ps-regs'][d['map']]['off'] + 8*d['index']: d
               for d in adt['devices'] if d.get('map') and
               not adt['ps-regs'][d['map']].get('reg', 0)}
    rows = []
    for path, node in nodes.items():
        if '/power-management@20e000000/' not in path:
            continue
        label = node.props.get('label', b'').rstrip(b'\0').decode()
        if not label or 'reg' not in node.props:
            continue
        offset = cells(node.props['reg'])[0]
        phandle = fdt.phandle_of(node)
        consumers = []
        for consumer_path, consumer in nodes.items():
            if phandle and phandle in cells(consumer.props.get('power-domains', b'')):
                consumers.append({'path': consumer_path,
                                  'status': consumer.props.get('status', b'okay').rstrip(b'\0').decode()})
        device = devices.get(offset)
        assert device is not None, (path, hex(offset))
        rows.append({'label': label, 'offset': hex(offset),
                     'physical': hex(0x20e000000 + offset), 'adt': device,
                     'always_on': 'apple,always-on' in node.props,
                     'direct_consumers': consumers})
        if args.cold_log:
            rows[-1]['cold_raw_before_floor'] = cold.get(offset)
            rows[-1]['warm_raw_before_floor'] = warm.get(offset)
    assert any(r['label'] == 'pms' and r['offset'] == '0x80120' for r in rows)
    print(json.dumps({'dtb_sha256': hashlib.sha256(blob).hexdigest(),
                      'note': 'DT topology only; no live register values or guaranteed probe order',
                      'domains': rows}, indent=2))


if __name__ == '__main__':
    main()
