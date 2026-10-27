#!/usr/bin/env python3
"""Pinned offline CPU/wake instruction evidence; never connects to the iPad."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

from capstone import Cs, CS_ARCH_ARM64

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/audio'))
import kcmap


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--ghidra', action='store_true', help='also retain focused shared-server evidence')
    args = ap.parse_args()
    out = ROOT / 'build/periph/aop-stage2d/re'
    out.mkdir(parents=True, exist_ok=True)
    kc, kernel, _ = kcmap.load()
    assert hashlib.sha256(kc).hexdigest() == 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
    decoder = Cs(CS_ARCH_ARM64, 0)
    for name, address, length in (
        ('provider-start', 0xfffffff005b4b738, 0x1c4),
        ('provider-stop', 0xfffffff005b4b8fc, 0x1d0),
        ('cpu-wake', 0xfffffff005b4e7c8, 0x88),
        ('rtbuddy-send', 0xfffffff005b66aa8, 0x180),
        ('v2-readreg', 0xfffffff005b4e9b4, 0x34),
        ('v1-running', 0xfffffff005b4f6c4, 0x10),
        ('idle-test', 0xfffffff005b4c460, 0xc0),
        ('rtbuddy-nmi', 0xfffffff005b66f14, 0x198),
    ):
        raw = kcmap.checked(kc, kcmap.va_offset(kernel, address, length), length)
        lines = [f'{i.address:016x} {i.mnemonic} {i.op_str}' for i in decoder.disasm(raw, address)]
        (out / f'{name}.asm').write_text('\n'.join(lines) + '\n')
    address, length = 0xfffffff006d3ff98, 0x700
    raw = kcmap.checked(kc, kcmap.va_offset(kernel, address, length), length)
    (out / 'vtable.txt').write_text(''.join(f'{i:03x} {struct.unpack_from("<Q", raw, i)[0]:016x}\n'
                                        for i in range(0x540, length, 8)))
    address, length = 0xfffffff005b62000, 0xf000
    raw = kcmap.checked(kc, kcmap.va_offset(kernel, address, length), length)
    (out / 'wake-call-sites.txt').write_text('\n'.join(
        f'{i.address:016x} {i.mnemonic} {i.op_str}' for i in decoder.disasm(raw, address)
        if i.mnemonic == 'ldr' and '#0x558]' in i.op_str) + '\n')
    if args.ghidra:
        def query(tool, *params):
            return subprocess.check_output([sys.executable, '-B', str(ROOT / 'scripts/ghidra.py'),
                                            tool, *params], text=True, timeout=60)
        identity = query('get_program_info')
        assert json.loads(identity)['sha256'] == hashlib.sha256(kc).hexdigest()
        (out / 'program.json').write_text(identity)
        for name, address in (('cpu-release', 'fffffff005b4e7c8'),
                              ('rtbuddy-nmi-tail', 'fffffff005b66f10'),
                              ('nmi-panic', 'fffffff005b7e36c')):
            result = query('decompile_function_by_address', f'address={address}')
            (out / f'{name}.json').write_text(result)
            (out / f'{name}.c').write_text(json.loads(result)['code'])
        (out / 'nmi-strings.json').write_text(query('list_strings', 'filter=NMI', 'limit=30'))
    print(out)


if __name__ == '__main__':
    main()
