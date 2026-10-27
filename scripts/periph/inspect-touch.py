#!/usr/bin/env python3
"""Preserve the J71t N1 constructed firmware asset for offline recon only.

This does not create a Linux touch container or upload anything to hardware.
"""
import hashlib
import json
import os
from pathlib import Path
import plistlib
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = ROOT / 'build/firmware/sources/usr/share/firmware/multitouch/J71t.mtprops'
    raw = source.read_bytes()
    personality = plistlib.loads(raw)['C1F15,1']
    firmware = personality['Constructed Firmware']
    if not isinstance(firmware, bytes) or not firmware:
        raise ValueError('missing constructed firmware')
    os.umask(0o077)
    output = ROOT / 'build/periph/touch'
    output.mkdir(parents=True, exist_ok=True)
    path = output / 'j71t-c1f15-1-constructed.bin'
    if path.exists() and path.read_bytes() != firmware:
        raise ValueError('refusing to overwrite different touch asset')
    path.write_bytes(firmware)
    path.chmod(0o600)
    report = {'source': str(source.relative_to(ROOT)),
              'source_sha256': hashlib.sha256(raw).hexdigest(),
              'personality': 'C1F15,1',
              'version': personality['Constructed Firmware Version'],
              'packet_type': personality['PreconstructedBootloadPacketType'],
              'reset_interval': personality['ResetInterval'],
              'output': str(path.relative_to(ROOT)), 'size': len(firmware),
              'sha256': hashlib.sha256(firmware).hexdigest(),
              'claim': 'verified raw plist asset; Linux N1 format/transport still unimplemented'}
    (output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    (output / 'manifest.json').chmod(0o600)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError) as error:
        sys.exit(str(error))
