#!/usr/bin/env python3
"""Pinned, offline mailbox/management instruction windows for the s2b study.

Complements the shared Ghidra decompilation: tiny tail-shared accessors have
no function body there. No target code execution or device connection.
"""
import hashlib
from pathlib import Path
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_ARCH_ARM, CS_MODE_THUMB

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/audio'))
import kcmap


def main():
    out = ROOT / 'build/periph/aop-stage2b/re'
    out.mkdir(parents=True, exist_ok=True)
    kc, kernel, _ = kcmap.load()
    assert hashlib.sha256(kc).hexdigest() == 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
    fw = (ROOT / 'build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin').read_bytes()
    assert hashlib.sha256(fw).hexdigest() == 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8'
    windows = (
        ('kc-fifo', 0xfffffff005b4e498, 0x20),
        ('kc-status-masks', 0xfffffff005b4e4b8, 0x120),
        ('kc-cpu-release', 0xfffffff005b4e7c8, 0x88),
        ('fw-management-hello', 0x010311ac, 0x15c),
        ('fw-management-power', 0x0103131c, 0xdc),
        ('fw-power-decode', 0x010314f4, 0x44),
        ('fw-power-encode', 0x01031408, 0x3c),
        ('fw-management-state', 0x01030f00, 0x2ac),
        ('fw-power-state', 0x01032ed0, 0x124),
    )
    for name, address, length in windows:
        if name.startswith('kc-'):
            raw = kcmap.checked(kc, kcmap.va_offset(kernel, address, length), length)
            decoder = Cs(CS_ARCH_ARM64, 0)
        else:
            raw = fw[address - 0x01000000:address - 0x01000000 + length]
            decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        lines = [f'{i.address:016x} {i.bytes.hex():12s} {i.mnemonic} {i.op_str}'
                 for i in decoder.disasm(raw, address)]
        (out / f'{name}.asm').write_text('\n'.join(lines) + '\n')
    # Locate callers, without mistaking a computed-switch disassembly for C.
    decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    decoder.skipdata = True
    targets = {0x010311ac, 0x0103131c, 0x010314f4, 0x01032ed0}
    calls = []
    for i in decoder.disasm(fw[0x4e0:0x389b8], 0x010004e0):
        if i.mnemonic in ('bl', 'b.w') and i.op_str.startswith('#0x'):
            if int(i.op_str[1:], 16) in targets:
                calls.append(f'{i.address:08x} {i.mnemonic} {i.op_str}')
    (out / 'fw-callers.txt').write_text('\n'.join(calls) + '\n')
    print(f'Pinned kernelcache/firmware instruction windows: {out}')


if __name__ == '__main__':
    main()
