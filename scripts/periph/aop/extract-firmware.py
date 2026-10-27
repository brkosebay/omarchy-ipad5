#!/usr/bin/env python3
"""Offline, pinned 20H350 j71t AOP extraction. Never contacts the iPad.

Default input is the already-downloaded partial IPSW component. --ipsw accepts
a local full IPSW. Output is a staging tree, not the host's /lib/firmware.
OSSerialize token format reference: Apple xnu-8796.141.3 OSSerializeBinary.cpp.
The extracted images are NOT directly bootable until the memory contract and
patchbay have been established. No calibration is manufactured here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[3]
MEMBER = 'Firmware/AOP/aopfw-ipad6faop.im4p'
COMPONENT_SHA = '5a4d8378bb83e175fc589cc1cf978430639d7ffcef07b2d94e9d6a4801124efa'
BUNDLE_SHA = '1306381a3876ede15ad0c3f77f6d1edb5273b1c1e93dff01ef3f6a1478327911'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def im4p_payload(raw):
    """Strict DER for this uncompressed IM4P, without an ipsw dependency."""
    def item(pos, expected):
        if pos + 2 > len(raw) or raw[pos] != expected:
            raise ValueError('unexpected/truncated DER tag')
        size = raw[pos + 1]
        pos += 2
        if size & 0x80:
            count = size & 0x7f
            if not 1 <= count <= 4 or pos + count > len(raw):
                raise ValueError('invalid DER length')
            size = int.from_bytes(raw[pos:pos + count], 'big')
            pos += count
        if pos + size > len(raw):
            raise ValueError('truncated DER payload')
        return raw[pos:pos + size], pos + size, pos

    _, end, pos = item(0, 0x30)
    if end != len(raw):
        raise ValueError('trailing IM4P bytes')
    for expected in (b'IM4P', b'aopf', b'1'):
        value, pos, _ = item(pos, 0x16)
        if value != expected:
            raise ValueError('wrong IM4P component/type/version')
    value, pos, _ = item(pos, 4)
    if pos != end:
        raise ValueError('unexpected IM4P fields')
    return value


class OSSerialize:
    """Bounded subset needed by the pinned bundle; reject cycles/duplicates."""
    def __init__(self, raw):
        if raw[:4] != b'\xd3\0\0\0' or len(raw) > 1024 * 1024:
            raise ValueError('not a bounded unindexed OSSerialize bundle')
        self.raw, self.pos, self.objects = raw, 4, []

    def take(self, size):
        end = self.pos + ((size + 3) & ~3)
        if end > len(self.raw):
            raise ValueError('truncated OSSerialize data')
        value = self.raw[self.pos:self.pos + size]
        self.pos = end
        return value

    def parse(self, depth=0):
        if depth > 24 or len(self.objects) > 4096:
            raise ValueError('OSSerialize complexity limit')
        word, = struct.unpack('<I', self.take(4))
        kind, count, last = word & 0x7f000000, word & 0xffffff, bool(word >> 31)
        if kind == 0x0c000000:
            if count >= len(self.objects) or self.objects[count] is None:
                raise ValueError('forward/cyclic object reference')
            return self.objects[count], last
        index = len(self.objects)
        self.objects.append(None)
        if kind in (0x01000000, 0x02000000):
            if count > 4096:
                raise ValueError('oversized collection')
            value = {} if kind == 0x01000000 else []
            for i in range(count):
                if isinstance(value, dict):
                    key, key_end = self.parse(depth + 1)
                    if key_end or not isinstance(key, str) or key in value:
                        raise ValueError('invalid/duplicate dictionary key')
                    child, child_end = self.parse(depth + 1)
                    value[key] = child
                else:
                    child, child_end = self.parse(depth + 1)
                    value.append(child)
                if child_end != (i == count - 1):
                    raise ValueError('inconsistent collection end')
        elif kind in (0x08000000, 0x09000000, 0x0a000000):
            value = self.take(count)
            if kind != 0x0a000000:
                if kind == 0x08000000:
                    if not value.endswith(b'\0'):
                        raise ValueError('unterminated symbol')
                    value = value[:-1]
                value = value.decode('utf-8')
                if '\0' in value:
                    raise ValueError('embedded string NUL')
        elif kind == 0x04000000 and count in (8, 16, 32, 64):
            value, = struct.unpack('<Q', self.take(8))
            if value >= 1 << count:
                raise ValueError('number out of range')
        elif kind == 0x0b000000 and count in (0, 1):
            value = bool(count)
        else:
            raise ValueError(f'unsupported OSSerialize token {word:#x}')
        self.objects[index] = value
        return value, last

    def bundle(self):
        value, last = self.parse()
        if not last or self.pos != len(self.raw) or not isinstance(value, dict):
            raise ValueError('invalid root/trailing bundle bytes')
        return value


def summarize(value):
    if isinstance(value, bytes):
        return {'size': len(value), 'sha256': digest(value)}
    if isinstance(value, dict):
        return {k: summarize(v) for k, v in value.items()}
    if isinstance(value, list):
        return [summarize(v) for v in value]
    return value


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    source = ap.add_mutually_exclusive_group()
    source.add_argument('--component', type=Path, default=ROOT / 'build/periph/aop-r1/ipsw/20H350__iPad6,11/aopfw-ipad6faop.im4p')
    source.add_argument('--ipsw', type=Path)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/periph/aop-r2/firmware')
    args = ap.parse_args()
    if args.ipsw:
        with zipfile.ZipFile(args.ipsw) as archive:
            manifest = plistlib.loads(archive.read('BuildManifest.plist'))
            paths = {i['Manifest']['AOP']['Info']['Path']
                     for i in manifest['BuildIdentities'] if i['Info']['DeviceClass'] == 'j71tap'}
            if paths != {MEMBER} or manifest['ProductBuildVersion'] != '20H350':
                raise ValueError('wrong IPSW build or j71t AOP identity')
            raw = archive.read(MEMBER)
    else:
        raw = args.component.read_bytes()
    if digest(raw) != COMPONENT_SHA:
        raise ValueError('component SHA256 mismatch; only 20H350 j71t is supported')
    payload = im4p_payload(raw)
    if digest(payload) != BUNDLE_SHA:
        raise ValueError('bundle SHA256 mismatch')
    bundle = OSSerialize(payload).bundle()
    dest = args.out / 'apple/aop'
    dest.mkdir(parents=True, exist_ok=True)
    (dest / 'ipad6faop-20H350.osbin').write_bytes(payload)
    # Preserve the complete bundle and a metadata inventory before any loader
    # chooses an image. Neither _ext nor metadata is silently discarded.
    for image in bundle['firmware']:
        target = image['header']['Target']
        if target not in ('ipad6faop', 'ipad6faop_ext') or not isinstance(image['data'], bytes):
            raise ValueError('unexpected image target/schema')
        (dest / f'{target}-20H350.bin').write_bytes(image['data'])
    report = {'component_sha256': digest(raw), 'bundle_sha256': digest(payload),
              'source': str(args.ipsw or args.component), 'bundle': summarize(bundle),
              'bootable': False, 'blocked_on': 'AP/IOP memory reservation, remap and patchbay contract'}
    (dest / 'ipad6faop-20H350.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    print(f'Staged only: {dest}')


if __name__ == '__main__':
    main()
