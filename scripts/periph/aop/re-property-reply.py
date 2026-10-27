#!/usr/bin/env python3
"""Decode the enable-fix configure failure and export pinned iOS/FW evidence.

Host only. --refresh queries the existing shared Ghidra kernelcache; it never
replaces that program. fixture() supplies the exact saved AFK bytes to the C
production-code replay without checking private calibration into the repo.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
CAPTURE = ROOT / 'build/periph/aop-stage23/stage3-enablefix-full-20261025T170134Z'
FW_SHA = 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8'
KC_SHA = 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
CAPTURE_SHA = {
    'rings.txt': '8ef67dee5677a544646dbffbc2e796d4e273c21b78d3e84026d13febfbd221e7',
    'transcript.txt': 'cebc2f274af4810da7537b744bdb9a5ff3ed34e829b036c61b4963ef10a3c07e',
}
FUNCTIONS = {
    'ios-reply': 'fffffff005bad150',
    'ios-dispatch': 'fffffff005bacf58',
    'ios-set-property': 'fffffff005bad898',
    'ios-send-wait': 'fffffff005bae344',
    'ios-builder': 'fffffff005bae168',
    'ios-cal-entry': 'fffffff005b96b60',
    'ios-cal-table': 'fffffff005b96d70',
    'ios-calibration': 'fffffff005b9d9e4',
    'ios-rate': 'fffffff005b99ef0',
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_capture():
    for name, expected in CAPTURE_SHA.items():
        assert digest((CAPTURE / name).read_bytes()) == expected, name
    rings = {}
    current = None
    for line in (CAPTURE / 'rings.txt').read_text().splitlines():
        match = re.fullmatch(r'ep (\d+) ring (tx|rx|hist) size=0x([0-9a-f]+) valid=1', line)
        if match:
            ep, kind, size = match.groups()
            current = dict(size=int(size, 16), data=bytearray(), header={})
            rings[int(ep), kind] = current
        elif current is not None:
            match = re.fullmatch(r'  \+(00|40|80) ((?:[0-9a-f]{8} ?){4})', line)
            if match:
                current['header'][int(match[1], 16)] = int(match[2].split()[0], 16)
            match = re.fullmatch(r'  ([0-9a-f]{8}): ((?:[0-9a-f]{8} ?){4})', line)
            if match:
                assert len(current['data']) == int(match[1], 16)
                current['data'] += struct.pack('<4I', *(int(x, 16) for x in match[2].split()))
    events = []
    for line in (CAPTURE / 'transcript.txt').read_text().splitlines():
        match = re.fullmatch(r'(\d+) (\d+)\.(\d{9}) ([RT]) ep=0x([0-9a-f]{2}) ([0-9a-f]{14}) 0 phase=\S+', line)
        if match:
            index, seconds, ns, direction, ep, payload = match.groups()
            events.append(dict(index=int(index), ns=int(seconds) * 10**9 + int(ns),
                               direction=direction, ep=int(ep, 16), payload=int(payload, 16)))
    assert events[0]['index'] == 0 and len(events) == 380
    return rings, events


def packet(data, offset):
    magic, length, channel, word = struct.unpack_from('<4I', data, offset)
    assert magic == 0x20504f41 and length >= 40
    payload = data[offset + 16:offset + 16 + length]
    size, version, category, subtype, tag = struct.unpack_from('<IBBHH', payload, 16)
    assert version == 2 and length == 40 + size
    return dict(length=length, channel=channel, word=word, category=category,
                subtype=subtype, tag=tag, size=size, payload=payload[40:])


def fixture():
    rings, events = read_capture()
    lines = ['/* Generated from the 20261025T170134Z capture; not synthetic replies. */',
             'static const struct { u8 ep, kind; u32 offset, size, wptr; u8 data[256]; } captured_rings[] = {']
    for event in events:
        ep, payload = event['ep'], event['payload']
        kind = (payload >> 48) - 0x8a
        if event['direction'] != 'R' or ep < 32 or kind not in (0, 1, 2):
            continue
        ring = rings[ep, ('tx', 'rx', 'hist')[kind]]
        offset = ((payload >> 32) & 0xffff) * 64
        size = ((payload >> 16) & 0xffff) * 64
        assert size == ring['size'] and len(ring['data']) == 256
        lines.append(f'{{ {ep}, {kind}, {offset}, {size}, {ring["header"][128]}, {{' +
                     ','.join(f'0x{b:02x}' for b in ring['data']) + '} },')
    lines += ['};', 'static const struct { u64 ns, word; } captured_events[] = {']
    # Replay all app handshakes/notifications through the zero-status reply,
    # including 2185 followed by 2286. Subsequent successes are peer fixtures.
    for event in events:
        if event['direction'] == 'R' and event['ep'] >= 32 and event['index'] <= 253:
            lines.append(f'{{ {event["ns"]}ULL, 0x{event["ep"]:02x}{event["payload"]:014x}ULL }},')
    lines += ['};', '']
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--refresh', action='store_true')
    ap.add_argument('--annotate', action='store_true', help='name/comment the understood iOS reply handler')
    args = ap.parse_args()
    out = ROOT / 'build/periph/aop-stage3/re/property'
    out.mkdir(parents=True, exist_ok=True)
    rings, events = read_capture()
    tx = packet(rings[33, 'tx']['data'], 0)
    rx = packet(rings[33, 'rx']['data'], 0x80)
    assert (tx['channel'], tx['category'], tx['subtype'], tx['tag'], tx['size']) == (0x10000007, 0x10, 4, 1, 28)
    assert tx['payload'][:8] == struct.pack('<II', 0, 0x1a)
    # Compare the complete first calibration value with the private packaged file.
    cal = (ROOT / 'build/periph/aop-stage3/root-firmware/accel-calibration.bin').read_bytes()
    assert cal[:12] == b'A9AC' + struct.pack('<II', 1, 4)
    key, size = struct.unpack_from('<II', cal, 12)
    assert key == 0x1a and size == 20 and tx['payload'][8:] == cal[20:20 + size]
    entries = []
    cursor = 12
    for expected_key in (0x1a, 0x0b, 0x18, 0x19):
        entry_key, length = struct.unpack_from('<II', cal, cursor)
        assert entry_key == expected_key
        entries.append(dict(key=entry_key, bytes=length))
        cursor += 8 + length
    assert cursor == len(cal)
    assert (rx['channel'], rx['category'], rx['subtype'], rx['tag'], rx['size']) == (0x10000007, 0x20, 0x84, 1, 4)
    assert rx['payload'] == bytes(4)
    assert rings[33, 'tx']['header'][64] == rings[33, 'tx']['header'][128] == 0x80
    for name, address in FUNCTIONS.items():
        path = out / (name + '.json')
        if args.refresh or not path.exists():
            path.write_text(subprocess.check_output([sys.executable, '-B', str(ROOT / 'scripts/ghidra.py'),
                'decompile_function_by_address', f'address={address}'], text=True))
        result = json.loads(path.read_text())
        assert result['code'] and not result['truncated'], name
        (out / (name + '.c')).write_text(result['code'])
    from capstone import Cs, CS_ARCH_ARM, CS_ARCH_ARM64, CS_MODE_THUMB
    sys.path.insert(0, str(ROOT / 'tools/audio'))
    import kcmap
    kc, kernel, _ = kcmap.load()
    assert digest(kc) == KC_SHA
    fw = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin').read_bytes()
    assert digest(fw) == FW_SHA
    for name, address, length in (
            ('fw-reply-builder', 0x1011d1c, 0x112),
            ('fw-subheader-builder', 0x10090a6, 0x34),
            ('ios-reply', 0xfffffff005bad150, 0xb8),
            ('ios-subheader-builder', 0xfffffff005bae308, 0x3c)):
        if name.startswith('fw-'):
            data = fw[address - 0x1000000:address - 0x1000000 + length]
            decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        else:
            data = kcmap.checked(kc, kcmap.va_offset(kernel, address, length), length)
            decoder = Cs(CS_ARCH_ARM64, 0)
        decoder.skipdata = True
        (out / (name + '.asm')).write_text(''.join(
            f'{i.address:016x} {i.bytes.hex():12s} {i.mnemonic} {i.op_str}\n'
            for i in decoder.disasm(data, address)))
    assert fw[0x11d6c:0x11d70].hex() == '41f08001'
    if args.annotate:
        for tool, kwargs in (
                ('rename_function_by_address', dict(function_address='fffffff005bad150',
                    new_name='AppleSPUInterface_completeTaggedReply')),
                ('set_decompiler_comment', dict(address='fffffff005bad150', comment=
                    'AOP stage3: reply category 0x20 dispatches here. Match subheader u16 tag at +8 to pending transaction; copy reply and wake gate. No requirement that reply subtype equal request. Pinned AOP FW 01011d6c ORs subtype with 0x80: property4 -> reply84. Device 20261025T170134Z ep33/channel10000007/tag1/status0 confirms.'))):
            print(subprocess.check_output([sys.executable, '-B', str(ROOT / 'scripts/ghidra.py'), tool,
                *(f'{k}={v}' for k, v in kwargs.items())], text=True).strip())
    report = dict(firmware_sha256=FW_SHA, kernelcache_sha256=KC_SHA,
                  captures={name: digest((CAPTURE / name).read_bytes()) for name in ('rings.txt', 'transcript.txt')},
                  request={k: v for k, v in tx.items() if k != 'payload'},
                  reply={k: v for k, v in rx.items() if k != 'payload'},
                  key=key, calibration_bytes=size, calibration_matches=True, ioreturn=0,
                  calibration_entries=entries,
                  tx_drained=True, firmware='01011d6c: subtype | 0x80; 01011d5e: original tag',
                  ios='bad150: match tag, wake waiter; bae168: leading IOReturn',
                  device_test_of_fix=False)
    (out / 'evidence.json').write_text(json.dumps(report, indent=2) + '\n')
    private_fixture = out / 'capture-fixture.h'
    private_fixture.write_text(fixture())
    private_fixture.chmod(0o600)
    print('PASS captured key1a: ep33/channel10000007 notify4/tag1/20 bytes consumed; reply20/84/tag1/status0 discarded by old driver')
    print(f'PASS pinned firmware/iOS evidence: {out}')


if __name__ == '__main__':
    main()
