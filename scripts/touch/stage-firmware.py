#!/usr/bin/env python3
"""Stage this unit's N1 packet and MtCl, entirely offline and under build/."""
import hashlib
import json
import os
from pathlib import Path
import plistlib

ROOT = Path(__file__).resolve().parents[2]


def halfword32(data):
    return int.from_bytes(bytes((data[1], data[0], data[3], data[2])), 'little')


def validate_packet(data):
    if len(data) != 57016 or data[:4] != b'\x18\xe1\x30\x01':
        raise ValueError('not the checked J71t C1F15,1 packet')
    length = int.from_bytes(data[4:6], 'big') * 4
    if length + 16 != len(data) or halfword32(data[6:10]) != 0:
        raise ValueError('invalid firmware packet address/length')
    if sum(data[4:10]) != int.from_bytes(data[10:12], 'big'):
        raise ValueError('firmware header checksum mismatch')
    if sum(data[12:-4]) != halfword32(data[-4:]):
        raise ValueError('firmware payload checksum mismatch')
    digest = hashlib.sha256(data).hexdigest()
    if digest != '98f73566489fed958a04bb73b87f63d8abd6948b7ceacd89d1bc638e64b8998d':
        raise ValueError('firmware identity changed; review before staging')


def main():
    os.umask(0o077)
    source = ROOT / 'build/firmware/sources/usr/share/firmware/multitouch/J71t.mtprops'
    properties = plistlib.loads(source.read_bytes())['C1F15,1']
    packet = properties['Constructed Firmware']
    validate_packet(packet)
    calibration = (ROOT / 'build/firmware/private/MtCl.bin').read_bytes()
    if len(calibration) != 1024 or not any(calibration) or calibration == b'\xff' * 1024:
        raise ValueError('real 1024-byte same-unit MtCl is required')
    destination = ROOT / 'build/firmware/lib/firmware/apple'
    destination.mkdir(parents=True, exist_ok=True)
    entries = []
    for name, body in [('j71t-n1.bin', packet), ('j71t-n1-calibration.bin', calibration)]:
        path = destination / name
        if path.exists() and path.read_bytes() != body:
            raise ValueError(f'refusing to overwrite different private input: {path}')
        path.write_bytes(body)
        path.chmod(0o600)
        entries.append({'path': str(path.relative_to(ROOT)), 'size': len(body),
                        'sha256': hashlib.sha256(body).hexdigest()})
    report = {'personality': 'C1F15,1', 'version': properties['Constructed Firmware Version'],
              'format': 'unmodified iOS preconstructed N1/Z2 boot packet; no Linux Z2 wrapper',
              'calibration': 'same-unit private MtCl, loaded by request_firmware_direct; PxCl absent',
              'files': entries}
    output = ROOT / 'build/touch/firmware-manifest.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + '\n')
    output.chmod(0o600)
    print('PASS: N1 packet/header/payload checksums; same-unit MtCl staged (private files)')


if __name__ == '__main__':
    main()
