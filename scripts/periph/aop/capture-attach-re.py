#!/usr/bin/env python3
"""Capture attach evidence from the shared kernelcache; host only, no device IO.

Missing function entries are retained as errors, with bounded instruction bytes
from the same shared program for an extracted-kext fallback. Never switches the
shared program or starts another instance on its project.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
SHA = 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
FUNCTIONS = {
    'b4b738': 'provider-start', 'b4b8fc': 'provider-stop',
    'b4e498': 'fifo-read', 'b4e4a8': 'fifo-write',
    'b649f0': 'hibernate-state', 'b64b1c': 'boot-state-action',
    'b65b54': 'power-transition', 'b66aa8': 'send-word',
    'b661e4': 'create-endpoint', 'b68764': 'boot-properties',
    'b668ec': 'ap-ready-request', 'b6a904': 'endpoint-service-name',
    'b6f8a4': 'load-patch', 'b71018': 'management',
    'b71390': 'management-power-callback', 'b713f0': 'management-power-action',
    'b71504': 'ap-power-notification', 'b715d4': 'quiesce-watchdog',
    'b716d0': 'ap-power', 'b71750': 'iop-power-request',
    'b717a4': 'endpoint-control', 'b7196c': 'hello',
    'b71afc': 'epmap', 'b71e08': 'running-epmap-bitmap',
    'b71e7c': 'watchdog-timeout', 'b71f1c': 'ping',
    'b71fc4': 'quiesced-power-request', 'b67264': 'allocate-buffer',
    'ba7984': 'spu-start', 'ba88e0': 'afk-switch',
    'ba9754': 'afk-start', 'ba9808': 'afk-stop', 'ba84d0': 'discard-stale-packets',
    'baaf2c': 'queue-constructor', 'b8465c': 'ring-constructor-wrapper',
    'bab1c8': 'ring-initializer', 'baba00': 'ring-reader',
    'bab540': 'ring-writer', 'babd00': 'ring-read-commit',
    'ba8554': 'ready-report', 'ba8398': 'channel-tx',
    'bae168': 'property-subheader', 'baf0fc': 'outer-header',
    'b99ef0': 'rate', 'b9d9e4': 'calibration', 'b9eb5c': 'motion-report',
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, default=ROOT / 'build/periph/aop-attach/re')
    args = ap.parse_args()
    out = args.output.resolve()
    out.relative_to(ROOT / 'build/periph')
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, GHIDRA_MCP_TIMEOUT='45')

    def query(name, *arguments):
        return subprocess.run([sys.executable, '-B', str(ROOT / 'scripts/ghidra.py'),
                               name, *arguments], text=True, capture_output=True,
                              timeout=60, env=env)

    identity = query('get_program_info')
    identity.check_returncode()
    info = json.loads(identity.stdout)
    if info['sha256'] != SHA:
        raise SystemExit('Refusing another shared Ghidra program')
    (out / 'program.json').write_text(identity.stdout)
    missing = []
    for suffix, label in FUNCTIONS.items():
        address = '0xfffffff005' + suffix
        result = query('decompile_function_by_address', 'address=' + address)
        stem = out / f'{suffix}-{label}'
        if result.returncode:
            missing.append(address)
            stem.with_suffix('.error.txt').write_text(result.stdout + result.stderr)
            result = query('read_memory', 'address=' + address, 'length=256')
            result.check_returncode()
            stem.with_suffix('.bytes.json').write_text(result.stdout)
        else:
            stem.with_suffix('.json').write_text(result.stdout)
            stem.with_suffix('.c').write_text(json.loads(result.stdout)['code'])
    for address, length, label in (
            ('0xfffffff0053110a0', 32, 'power-requests'),
            ('0xfffffff006d5e7f8', 144, 'calibration-table'),
            ('0xfffffff005b4e498', 32, 'fifo-accessors')):
        result = query('read_memory', 'address=' + address, f'length={length}')
        result.check_returncode()
        (out / f'{label}.json').write_text(result.stdout)
    for address, label in (('0xfffffff005b6a904', 'endpoint-service-name'),
                           ('0xfffffff005b7196c', 'hello-state-gate')):
        result = query('disassemble_function', 'address=' + address, 'limit=120')
        result.check_returncode()
        (out / f'{label}.asm.json').write_text(result.stdout)
    print(f'Captured {len(FUNCTIONS)} entries in {out}; missing definitions: {missing}')


if __name__ == '__main__':
    main()
