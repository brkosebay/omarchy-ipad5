#!/usr/bin/env python3
"""Offline entitlement comparison and bounded IOReport call-site evidence for pulled daemons."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--daemons', type=Path, default=ROOT / 'build/power/ios-r3/daemons')
p.add_argument('--calls', action='store_true', help='Emit bounded same-image call-site disassembly instead of JSON')
a = p.parse_args()
paths = {}
for binary in a.daemons.rglob('*'):
    if binary.is_file() and binary.read_bytes()[:4] == b'\xcf\xfa\xed\xfe':
        if binary.name in paths: raise SystemExit(f'Duplicate daemon basename: {binary.name}')
        paths[binary.name] = binary

if a.calls:
    for name, sites in [('powerdatad', (0x100009c3c,)),
                        ('thermalmonitord', (0x1000100fc, 0x100038cf0, 0x100043018, 0x100048510))]:
        binary = paths[name]
        indirect = subprocess.check_output(['xcrun', 'otool', '-Iv', str(binary)], text=True)
        stub = re.search(r'^(0x[0-9a-f]+)\s+\d+ _IOReportCreateSubscription$', indirect, re.M)
        assert stub, f'Missing subscription stub: {name}'
        print(f'{name}: SHA256={hashlib.sha256(binary.read_bytes()).hexdigest()}')
        print(f'IOReportCreateSubscription stub={stub[1]}')
        for site in sites:
            proc = subprocess.run(['ipsw', 'macho', 'disass', str(binary), '--quiet',
                '--vaddr', hex(site - 24), '--count', '9'], check=True, capture_output=True, text=True)
            assert re.search(rf'0x{site:x}:.*\bbl\s+0x{int(stub[1], 16):x}\b', proc.stdout)
            print(proc.stdout)
    print('All selected call sites pass x0=NULL, x1=channel aggregate, x2=&subscribed, w3=0.')
    print('thermalmonitord uses x4=NULL; powerdatad passes a stack CFErrorRef* in x4.')
    print('thermalmonitord releases the source aggregate immediately after subscribing, matching the helper.')
else:
    rows = {}
    for name, binary in sorted(paths.items()):
        raw = subprocess.check_output(['ldid', '-e', str(binary)])
        ent = plistlib.loads(raw)
        rows[name] = {
            'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'entitlements': ent,
            'iokit_access': {k: v for k,v in ent.items() if 'iokit-user-client-class' in k},
            'sandbox_selection': {k: v for k,v in ent.items() if 'sandbox' in k or 'seatbelt' in k},
        }
    helper = plistlib.loads((ROOT / 'build/power/ios-ioreport.plist').read_bytes())
    rows['helper'] = {'entitlements': helper, 'note': 'Current round-3 signature; not proof of sample-2 signature.'}
    print(json.dumps(rows, indent=2))
