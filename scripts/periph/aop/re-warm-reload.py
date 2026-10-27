#!/usr/bin/env python3
"""Pinned offline warm-shutdown evidence and real Thumb power-codec execution.

Run with: uv run --with capstone --with unicorn python -B <this file>
--refresh exports the shared iOS Ghidra program; never contacts the device.
The execution covers state encoding, not peripheral power or DMA success.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/periph/aop-stage3/re/warm'
CAL = runpy.run_path(str(Path(__file__).with_name('re-calibration.py')))
IOS = {
    'ios-power-transition': 'fffffff005b65b54',
    'ios-ap-power-callback': 'fffffff005b713f0',
    'ios-ap-power': 'fffffff005b71504',
    'ios-iop-power': 'fffffff005b71750',
    'ios-endpoint-run': 'fffffff005b717a4',
    'ios-cpu-run': 'fffffff005b4e7c8',
}


def codec(fw):
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
    from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    u.mem_map(0x1000000, 0x60000)
    u.mem_write(0x1000000, fw)
    u.mem_map(0x2000000, 0x2000)

    def call(address, value):
        u.reg_write(UC_ARM_REG_SP, 0x2001000)
        u.reg_write(UC_ARM_REG_LR, 0x2000001)
        u.reg_write(UC_ARM_REG_R0, value)
        u.emu_start(address | 1, 0x2000000, count=100)
        assert u.reg_read(UC_ARM_REG_PC) == 0x2000000
        return u.reg_read(UC_ARM_REG_R0)

    records = []
    for wire, state in ((0, 1), (1, 3), (0x201, 8), (0x202, 8), (0x10, 0x20), (0x20, 0x21)):
        assert call(0x10314f4, wire) == state
        encoded = call(0x1031408, state)
        assert encoded == (0x201 if wire == 0x202 else wire)
        records.append(dict(request=hex(wire), internal=hex(state), ack=hex(encoded)))
    return records


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--refresh', action='store_true')
    args = ap.parse_args()
    from capstone import Cs, CS_ARCH_ARM, CS_ARCH_ARM64, CS_MODE_THUMB
    sys.path.insert(0, str(ROOT / 'tools/audio'))
    import kcmap
    OUT.mkdir(parents=True, exist_ok=True)
    fw = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin').read_bytes()
    assert hashlib.sha256(fw).hexdigest() == CAL['FW_SHA']
    raw, kernel, _ = kcmap.load()
    assert hashlib.sha256(raw).hexdigest() == CAL['KC_SHA']
    for name, address in IOS.items():
        path = OUT / (name + '.json')
        if args.refresh or not path.exists():
            path.write_text(subprocess.check_output([sys.executable, '-B', str(ROOT / 'scripts/ghidra.py'),
                'decompile_function_by_address', f'address={address}'], text=True))
        result = json.loads(path.read_text())
        assert result['code'] and not result['truncated']
        (OUT / (name + '.c')).write_text(result['code'])
    decoder = Cs(CS_ARCH_ARM64, 0)
    for name, address, size in (
        ('ios-iop-power-table', 0xfffffff0053110a0, 32),
        ('ios-stop', 0xfffffff005b4b8fc, 0x15c),
        ('ios-ap-callback', 0xfffffff005b713f0, 0x114),
        ('ios-ap-sender', 0xfffffff005b71504, 0xd0),
        ('ios-iop-transition', 0xfffffff005b65b54, 0x4d8),
        ('ios-v2-unmap-stub', 0xfffffff005b4e7c4, 4),
        ('ios-v2-run', 0xfffffff005b4e7c8, 0x68)):
        data = kcmap.checked(raw, kcmap.va_offset(kernel, address, size), size)
        (OUT / (name + '.bin')).write_bytes(data)
        (OUT / (name + '.asm')).write_text(''.join(
            f'{i.address:016x} {i.bytes.hex()} {i.mnemonic} {i.op_str}\n'
            for i in decoder.disasm(data, address)))
        if name == 'ios-iop-power-table':
            assert struct.unpack('<4Q', data) == tuple(0x0060000000000000 | x for x in (0x201, 0x220, 0x202, 0))
    decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    for name, address, size in (
        ('fw-management', 0x103131c, 0xdc), ('fw-decode', 0x10314f4, 0x44),
        ('fw-encode', 0x1031408, 0x3c), ('fw-iop-ack', 0x1031444, 0x50),
        ('fw-ap-ack', 0x1031498, 0x5c), ('fw-transition', 0x1032c18, 0x2b8),
        ('fw-callback-walk', 0x1032bb6, 0x62), ('fw-power-task', 0x10329a8, 0x1d0)):
        data = fw[address - 0x1000000:address - 0x1000000 + size]
        (OUT / (name + '.asm')).write_text(''.join(
            f'{i.address:08x} {i.bytes.hex()} {i.mnemonic} {i.op_str}\n'
            for i in decoder.disasm(data, address)))
    captures = sorted((ROOT / 'docs/periph/logs').glob('20261025-18*-*state.txt'))
    report = dict(firmware_sha256=CAL['FW_SHA'], kernelcache_sha256=CAL['KC_SHA'],
        codec=codec(fw), hardware_success_verified=False,
        selected=dict(ap_request='00b0000000000010', ap_ack='00b0000000000010',
                      iop_request='0060000000000201', iop_ack='0070000000000201'),
        reasoning='Asahi AP-before-IOP quiesce ordering; A9 iOS resumable IOP sleep201; exact firmware codec/ACK paths',
        stronger_reset_found=False,
        reset_scope='bounded V2 stop/start/unmap/run investigation; not an exhaustive panic-recovery proof',
        excluded='bare OFF0, speculative wrapper/PMGR reset, SRAM/BSS scrub, shared ANS power cycle',
        captures={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in captures})
    (OUT / 'evidence.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS pinned A9 power codec: real Thumb decode/encode, including 10->20->10 and 201->8->201; iOS table/stop evidence exported')
    print('Hardware quiescence, reset sufficiency and a second streaming boot remain unverified.')


if __name__ == '__main__':
    main()
