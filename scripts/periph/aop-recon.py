#!/usr/bin/env python3
"""Offline AOP round-1 evidence collection; never opens a device connection.

  scripts/periph/aop-recon.py inventory
  scripts/periph/aop-recon.py ghidra TOOL key=value ...
  scripts/periph/aop-recon.py pointers 0xfffffff005ba8554
  scripts/periph/aop-recon.py words 0xfffffff006d5e7f8 0x90

Ghidra replies are retained under build/periph/aop-r1/ghidra, so subsequent
rounds can inspect exact decompilation/xref evidence without repeating queries.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/periph/aop-r1'
sys.path.insert(0, str(ROOT / 'tools/audio'))
import kcmap


def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')


def inventory():
    kc, kernel, entries = kcmap.load()
    selected = [e for e in entries if re.search(
        r'AppleA7IOP|AppleSPU|AppleAOP|RTBuddy|IOSlaveProcessor|AppleHIDEvent',
        e.get('CFBundleIdentifier', ''))]
    records = []
    for entry in selected:
        img = kcmap.image(kc, kernel, entry)
        records.append({'prelink': entry, 'image': img})
        lines = []
        for sec in kcmap.sections(img) if img else []:
            if sec['name'] not in ('__cstring', '__os_log'):
                continue
            raw = kcmap.checked(kc, kcmap.va_offset(kernel, sec['va'], sec['size']), sec['size'])
            for match in re.finditer(rb'[\x20-\x7e\t\n]{4,}', raw):
                lines.append(f"{sec['va'] + match.start():#x}\t{match.group().decode()!r}")
        (OUT / (entry['CFBundleIdentifier'] + '.strings')).write_text('\n'.join(lines) + '\n')
    result = {'kernelcache': str(kcmap.KC.relative_to(ROOT)),
              'sha256': hashlib.sha256(kc).hexdigest(), 'kexts': records}
    dump(OUT / 'inventory.json', result)
    for item in records:
        entry = item['prelink']
        print(entry['CFBundleIdentifier'], entry.get('CFBundleVersion'),
              hex(entry.get('_PrelinkExecutableLoadAddr', 0)))
        for name, personality in entry.get('IOKitPersonalities', {}).items():
            if re.search('AOP|SPU|A7IOPV1', name, re.I):
                print(' ', name, json.dumps(personality))


def ghidra(tool, arguments):
    spec = importlib.util.spec_from_file_location('aop_ghidra_client', ROOT / 'scripts/ghidra.py')
    client = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(client)
    args = {}
    for arg in arguments:
        key, sep, value = arg.partition('=')
        if not sep:
            raise ValueError('expected key=value')
        try:
            args[key] = json.loads(value)
        except ValueError:
            args[key] = value
    _, sid = client.post({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                         'params': {'protocolVersion': '2025-06-18', 'capabilities': {},
                                    'clientInfo': {'name': 'aop-recon', 'version': '1'}}})
    client.post({'jsonrpc': '2.0', 'method': 'notifications/initialized'}, sid)
    reply, _ = client.post({'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call',
                            'params': {'name': tool, 'arguments': args}}, sid)
    if not reply or 'error' in reply:
        raise RuntimeError(reply)
    result = reply['result']
    if 'structuredContent' in result:
        data = result['structuredContent']
    else:
        content = '\n'.join(c.get('text', '') for c in result.get('content', []))
        try:
            data = json.loads(content)
        except ValueError:
            data = {'text': content}
    label = re.sub(r'[^a-zA-Z0-9_.-]+', '_', '_'.join([tool, *arguments]))[:200]
    dump(OUT / 'ghidra' / (label + '.json'), {'tool': tool, 'arguments': args, 'result': data})
    if isinstance(data, dict) and data.get('code'):
        (OUT / 'ghidra' / (label + '.c')).write_text(data['code'])
    print(json.dumps(data, indent=2))
    if result.get('isError'):
        raise SystemExit(1)


def pointers(target):
    kc, kernel, _ = kcmap.load()
    needle = struct.pack('<Q', int(target, 0))
    for seg in kernel['segments']:
        raw = kcmap.checked(kc, seg['fileoff'], seg['filesize'])
        start = 0
        while (offset := raw.find(needle, start)) >= 0:
            if offset % 8 == 0:
                print(hex(seg['va'] + offset), seg['name'])
            start = offset + 1


def words(address, length):
    kc, kernel, _ = kcmap.load()
    address, length = int(address, 0), int(length, 0)
    raw = kcmap.checked(kc, kcmap.va_offset(kernel, address, length), length)
    for offset in range(0, len(raw) - 7, 8):
        value, = struct.unpack_from('<Q', raw, offset)
        suffix = ''
        try:
            pos = kcmap.va_offset(kernel, value, 1)
            string = kc[pos:pos+160].split(b'\0', 1)[0]
            if len(string) >= 3 and all(c in (9, 10) or 32 <= c < 127 for c in string):
                suffix = ' ' + repr(string.decode())
        except ValueError:
            pass
        print(f'{address+offset:#x} +{offset:#05x}: {value:#018x}{suffix}')


def verify():
    """Cross-check report anchors against the exact binaries, not cached prose."""
    kc, kernel, entries = kcmap.load()
    assert hashlib.sha256(kc).hexdigest() == 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
    bundles = {e['CFBundleIdentifier']: e for e in entries}
    personalities = bundles['com.apple.driver.AppleA7IOP']['IOKitPersonalities']
    assert personalities['AppleA7IOPV2']['IONameMatch'] == 'iop,s8000'
    assert bundles['com.apple.driver.RTBuddy']['IOKitPersonalities']['RTBuddy']['IONameMatch'] == 'iop-nub,rtbuddy'

    def read(va, size):
        return kcmap.checked(kc, kcmap.va_offset(kernel, va, size), size)

    # AArch64 unsigned-offset LDR/STR X encodings independently establish
    # FIFO access width and offsets, even with a missing Ghidra function.
    assert struct.unpack('<I', read(0xfffffff005b4e49c, 4))[0] == (0xf9400000 | (0x4038 // 8) << 10 | 8 << 5 | 8)
    assert struct.unpack('<I', read(0xfffffff005b4e4b0, 4))[0] == (0xf9000000 | (0x4010 // 8) << 10 | 9 << 5 | 8)
    expected = [('low-temp-accel-offset', 0x1a), ('accel-sensitivity-calibration', 0xb),
                ('accel-range-sensitivity-cal', 0x27), ('accel-orientation', 0x18),
                ('accel-interrupt-calibration', 0x19), ('accel-range-intr-cal', 0x4b),
                ('accel-nominal-extr-cal', 0x4c), ('accel-range-extr-cal', 0x4d),
                ('accel-offset-cal', 0x1a)]
    for i, (name, tag) in enumerate(expected):
        ptr, value = struct.unpack('<QQ', read(0xfffffff006d5e7f8 + 16*i, 16))
        assert read(ptr, len(name)+1) == name.encode() + b'\0'
        assert value & 0xffffffff == tag
    fwdir = OUT / 'ipsw/20H350__iPad6,11'
    manifest = plistlib.loads((fwdir / 'BuildManifest.plist').read_bytes())
    assert (manifest['ProductVersion'], manifest['ProductBuildVersion']) == ('16.7.10', '20H350')
    identities = [x for x in manifest['BuildIdentities'] if x['Info']['DeviceClass'] == 'j71tap']
    assert len(identities) == 2
    for identity in identities:
        info = identity['Manifest']['AOP']['Info']
        assert info['Path'] == 'Firmware/AOP/aopfw-ipad6faop.im4p'
        assert info['IsFUDFirmware'] and not info['IsLoadedByiBoot'] and not info['IsLoadedByiBootStage1']
    fw = (fwdir / 'aopfw-ipad6faop.im4p').read_bytes()
    assert hashlib.sha256(fw).hexdigest() == '5a4d8378bb83e175fc589cc1cf978430639d7ffcef07b2d94e9d6a4801124efa'
    payload = (OUT / 'aop.bin').read_bytes()
    assert hashlib.sha256(payload).hexdigest() == '1306381a3876ede15ad0c3f77f6d1edb5273b1c1e93dff01ef3f6a1478327911'
    for value in (b'ipad6faop', b'RTKit-2062.142.1.release'):
        assert value in payload
    assert b'ipad6faop' not in kc
    print('PASS: kernelcache identity, kext matching, 64-bit FIFO opcodes, 9 calibration tags, 2 j71t manifest identities and exact firmware hashes')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=('inventory', 'ghidra', 'pointers', 'words', 'verify'))
    ap.add_argument('arguments', nargs='*')
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.action == 'inventory':
        inventory()
    elif args.action == 'verify':
        verify()
    elif args.action == 'ghidra':
        ghidra(args.arguments[0], args.arguments[1:])
    elif args.action == 'pointers':
        pointers(args.arguments[0])
    else:
        words(*args.arguments)


if __name__ == '__main__':
    main()
