#!/usr/bin/env python3
"""Orchestrator-only fixed-address, read-only S8003 thermal register capture.

No writes, no address argument, no scans. Run at stock clocks on Linux. This
reads only known ADT thermal control/configuration/result registers. If strict
devmem refuses a range, report the error; do not relax kernel protections.
"""
import json
import os
import platform
import struct
import time

if platform.system() != 'Linux' or os.geteuid() != 0:
    raise SystemExit('Run as root on the iPad Linux system through the orchestrator')
fd = os.open('/dev/mem', os.O_RDONLY | os.O_CLOEXEC)
try:
    result = {'epoch_ns': time.time_ns(), 'hardware_writes': 'none', 'sensors': {}}
    for index, offset in enumerate((0, 0x100, 0x200, 0, 0x40, 0x200)):
        tcc = index >= 3
        base = (0x202230000 if tcc else 0x20e0c0000) + offset
        registers = {'ctl0': 0, 'ctl1': 8, 'ctl2': 16 if tcc else 12,
                     'bk': 32 if tcc else 24, 'bk1': 40 if tcc else 28,
                     'sum': 48 if tcc else 32, 'count': 56 if tcc else 36}
        if tcc:
            registers['params'] = 24
        row = {'base': hex(base)}
        for name, register in registers.items():
            try:
                raw = os.pread(fd, 4, base + register)
                if len(raw) != 4:
                    raise OSError('short register read')
                row[name] = f'{struct.unpack("<I", raw)[0]:08x}'
            except OSError as exc:
                row[name + '_error'] = str(exc)
        result['sensors'][f'apple-a9-{"tcc" if tcc else "soc"}{index % 3}'] = row
    print(json.dumps(result, indent=2))
finally:
    os.close(fd)
