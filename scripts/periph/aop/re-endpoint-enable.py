#!/usr/bin/env python3
"""Export pinned routing-enable evidence from firmware/KC and frozen HELLO SRAM.

Host only: no device IO, shared Ghidra replacement, or firmware modification.
Instruction windows retain literal pools/computed switches; prefer the assembly
to the older firmware decompiler's merged f27c/f5dc function body.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_ARCH_ARM, CS_MODE_THUMB

ROOT = Path(__file__).resolve().parents[3]
BASE = 0x1000000
FW_SHA = 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8'
KC_SHA = 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
CAPTURE = ROOT / 'build/periph/aop-stage23/stage3-pingfix-full-20261025T164218Z'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def word(data, address):
    assert BASE <= address <= BASE + len(data) - 4, hex(address)
    return struct.unpack_from('<I', data, address - BASE)[0]


def string(data, address):
    assert BASE <= address < BASE + len(data), hex(address)
    start = address - BASE
    return data[start:data.index(b'\0', start)].decode('ascii')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--refresh', action='store_true', help='also query shared Ghidra iOS functions')
    args = ap.parse_args()
    out = ROOT / 'build/periph/aop-stage3/re/enable'
    out.mkdir(parents=True, exist_ok=True)
    fw = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin').read_bytes()
    assert digest(fw) == FW_SHA
    sys.path.insert(0, str(ROOT / 'tools/audio'))
    import kcmap
    kc, kernel, _ = kcmap.load()
    assert digest(kc) == KC_SHA
    for name, address, size in (
            ('fw-source-init-enable-wait', 0x100f27c, 0x148),
            ('fw-source-init-completion-wait', 0x1011e74, 0x1e),
            ('fw-management-start-enable', 0x103131c, 0xdc),
            ('fw-routing-enable', 0x102b330, 0x44),
            ('fw-routing-enabled-test', 0x102b374, 0x38),
            ('fw-routing-lookup', 0x102ad10, 0x24),
            ('fw-afk-state-machine', 0x100f5dc, 0x288),
            ('ios-event-source-enable', 0xfffffff005b1fa24, 0x58),
            ('ios-endpoint-control-gate', 0xfffffff005b67528, 0x94),
            ('ios-management-startep', 0xfffffff005b717a4, 0x5c)):
        if name.startswith('fw-'):
            data = fw[address - BASE:address - BASE + size]
            decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        else:
            data = kcmap.checked(kc, kcmap.va_offset(kernel, address, size), size)
            decoder = Cs(CS_ARCH_ARM64, 0)
        decoder.skipdata = True
        (out / (name + '.asm')).write_text(''.join(
            f'{i.address:016x} {i.bytes.hex():12s} {i.mnemonic} {i.op_str}\n'
            for i in decoder.disasm(data, address)))
    # The immutable image contains the routing-name table; it is not an ADT
    # guess. f27c resolves source names here and stores routing at object+a4.
    transport = word(fw, 0x100f3a4)
    assert word(fw, transport) == 0 and word(fw, transport + 4) == 0
    table = word(fw, transport + 8)
    endpoints = {}
    for address in range(table + 8, table + 8 + 32 * 16, 16):
        name = word(fw, address + 12)
        if not name:
            break
        endpoints[string(fw, name)] = word(fw, address)
    assert endpoints['accel'] == 0x21 and endpoints['wakehint'] == 0x2c
    vtable = 0xfffffff006d4c000
    at = kcmap.va_offset(kernel, vtable + 0x578, 8)
    assert struct.unpack_from('<Q', kc, at)[0] == 0xfffffff005b67528
    # This snapshot is from HELLO, before the later logged stall. Read only
    # construction links/names, never claim these are sampled execution PCs.
    frozen = (CAPTURE / 'cold_after.bin').read_bytes()
    assert len(frozen) == 0x9b000
    order = []
    source = 0x1069c20
    while len(order) < 32:
        name_ptr = word(frozen, source + 0x14)
        if not BASE <= name_ptr < BASE + len(fw):
            break
        name = string(fw, name_ptr)
        if name not in endpoints or endpoints[name] < 0x20:
            break
        assert name not in [item['name'] for item in order]
        order.append(dict(source=hex(source), name=name, routing=hex(endpoints[name])))
        source = word(frozen, source + 0x20)
    assert len(order) == 17 and order[0]['name'] == 'wakehint'
    expected = [0x2c, 0x2b, 0x29, 0x20, 0x26, 0x25, 0x21, 0x32, 0x2d,
                0x2a, 0x28, 0x31, 0x30, 0x27, 0x24, 0x23, 0x22]
    assert [int(item['routing'], 16) for item in order] == expected
    excerpt = (ROOT / 'docs/periph/logs/20261025-1750-aop-stage3-pingfix-full-dmesg-excerpt.txt').read_text()
    assert 'T 0050002100000002 status=0' in excerpt
    assert 'waiting for endpoint enable, _routing=2c' in excerpt
    assert 'afk-init-v8' not in excerpt and 'failed-no-accel-service' in excerpt
    report = dict(firmware_sha256=FW_SHA, kernelcache_sha256=KC_SHA,
                  routing_names=endpoints, source_construction_chain=order,
                  snapshot_sha256=digest(frozen), snapshot_event='HELLO, not failure-time PC',
                  ios_enable='b1fa24 -> RTBuddy vtable +578 -> b67528/b675a0 -> b717a4',
                  enable_word='0050002c00000002 on management endpoint 0',
                  firmware_enable='3131c type5 low32=2 -> 2b330 sets routing bit0',
                  firmware_wait='f27c -> 2b374 tests bit0 before AFK INIT v8',
                  completion_wait='11e74 waits source+24 until f5dc AFK state8',
                  chosen_fix='enable advertised apps before service wait; negotiate each AFK source',
                  device_test_of_fix=False)
    (out / 'evidence.json').write_text(json.dumps(report, indent=2) + '\n')
    if args.refresh:
        for name, address in (('ios-event-source-enable', 'fffffff005b1fa24'),
                              ('ios-management-startep', 'fffffff005b717a4')):
            result = subprocess.check_output([str(ROOT / 'scripts/ghidra.py'),
                'decompile_function_by_address', 'address=' + address], text=True)
            (out / (name + '.json')).write_text(result)
    print(f'PASS pinned routing0x2c=wakehint, iOS STARTEP enable, serialized AFK dependency; {out}')


if __name__ == '__main__':
    main()
