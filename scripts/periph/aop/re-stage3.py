#!/usr/bin/env python3
"""Export shared-kernelcache stage3 protocol evidence; never runs the device.

Uses the existing Ghidra server through ghidra.py. Cached exports are reused;
--refresh re-queries them. --annotate names only the functions established here.
The unanalysed crash/AFK entries use Ghidra read_memory plus local Capstone.
"""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/periph/aop-stage3/re'
FUNCTIONS = {
    'create-endpoint': 'fffffff005b661e4',
    'epmap': 'fffffff005b71afc',
    'ap-power': 'fffffff005b71504',
    'iop-power': 'fffffff005b71750',
    'ios-ready': 'fffffff005b716d0',
    'syslog-init': 'fffffff005b7653c',
    'syslog-rx': 'fffffff005b765e4',
    'syslog-getbuf': 'fffffff005b767f8',
    'syslog-slot': 'fffffff005b7698c',
    'ioreport-init': 'fffffff005b6af84',
    'ioreport-rx': 'fffffff005b6b268',
    'endpoint-name': 'fffffff005b6a904',
}
NAMES = {
    'syslog-init': 'RTBuddySyslog_init',
    'syslog-rx': 'RTBuddySyslog_handleMessage',
    'syslog-getbuf': 'RTBuddySyslog_allocateBuffer',
    'syslog-slot': 'RTBuddySyslog_readSlot',
    'ioreport-init': 'RTBuddyIOReport_init',
    'ioreport-rx': 'RTBuddyIOReport_handleMessage',
    'ios-ready': 'RTBuddyManagement_notifyAPReady',
}


def call(tool, **kwargs):
    return subprocess.check_output(
        [str(ROOT / 'scripts/ghidra.py'), tool,
         *(f'{k}={v}' for k, v in kwargs.items())], text=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--refresh', action='store_true')
    ap.add_argument('--annotate', action='store_true')
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    for name, address in FUNCTIONS.items():
        path = OUT / (name + '.txt')
        if args.refresh or not path.exists():
            path.write_text(call('decompile_function_by_address', address=address))
        result = json.loads(path.read_text())
        assert result['code'] and not result['truncated'], name
        if args.annotate and name in NAMES:
            print(call('rename_function_by_address', function_address=address,
                       new_name=NAMES[name]).strip())
    for name, address, length in (
            ('crashlog-rx', 'fffffff005b6d5f8', 832),
            ('afk-rx', 'fffffff005ba88e0', 0x624)):
        import capstone
        path = OUT / (name + '-bytes.json')
        if args.refresh or not path.exists():
            path.write_text(call('read_memory', address=address, length=length))
        data = json.loads(path.read_text())
        assert not data['gaps'] and data['length'] == length
        lines = [f'{i.address:016x}: {i.mnemonic} {i.op_str}' for i in
                 capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM).disasm(
                     bytes.fromhex(data['hex']), int(address, 16))]
        (OUT / (name + '.asm')).write_text('\n'.join(lines) + '\n')
    print(f'PASS kernelcache evidence exported to {OUT}; see docs/periph/aop-stage3.md')


if __name__ == '__main__':
    main()
