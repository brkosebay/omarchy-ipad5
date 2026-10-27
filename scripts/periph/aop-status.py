#!/usr/bin/env python3
"""Orchestrator-only j71t AOP status capture; never writes hardware.

--list works offline. Actual capture requires root on j71t Linux, after the
orchestrator establishes that the AOP wrapper is powered and accessible.
The FIFO data registers are deliberately absent: reading them consumes traffic.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import struct
import time

BASE = 0x210800000
REGISTERS = {
    'cpu_control': 0x44,
    'cpu_running': 0xb88,
    'inbox_control': 0x4008,
    'outbox_control': 0x4020,
    'idle_status': 0x4040,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--list', action='store_true', help='print allowlist without device access')
    args = ap.parse_args()
    addresses = {name: hex(BASE + offset) for name, offset in REGISTERS.items()}
    if args.list:
        print(json.dumps({'width_bits': 32, 'registers': addresses,
                          'excluded': 'FIFO data, set/clear, reset, firmware/remap, watchdog'}, indent=2))
        return
    if platform.system() != 'Linux' or os.geteuid() != 0:
        raise SystemExit('Capture requires root on j71t Linux; --list is host-only')
    compatible = Path('/sys/firmware/devicetree/base/compatible').read_bytes()
    if b'j71t' not in compatible.lower():
        raise SystemExit('Refusing capture: Linux DT compatible does not identify j71t')
    result = {'epoch_ns': time.time_ns(), 'hardware_writes': 'none', 'registers': {}}
    fd = os.open('/dev/mem', os.O_RDONLY | os.O_CLOEXEC)
    try:
        for name, address in addresses.items():
            try:
                raw = os.pread(fd, 4, int(address, 16))
                if len(raw) != 4:
                    raise OSError('short MMIO read')
                result['registers'][name] = {'address': address, 'value': hex(struct.unpack('<I', raw)[0])}
            except OSError as exc:
                result['registers'][name] = {'address': address, 'error': str(exc)}
                result['stopped_after_error'] = True
                break
    finally:
        os.close(fd)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
