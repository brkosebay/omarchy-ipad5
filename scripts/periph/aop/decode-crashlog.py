#!/usr/bin/env python3
"""Decode an AOP RTKit crashlog, offline/read-only.

Input: debugfs crashlog.bin (raw, preferred) or a dmesg/capture text with
'AOP crash: OFFSET: bytes' lines. Layout, as dumped on J71t: 'CLHE' header
(version +4, used size +8, flags +12), then sections with the fourcc at +0
and their total size at +12, data from +16. Section contents beyond strings
(Cver/Cstr) are printed as words; words inside the pinned firmware's linked
range 0x01000000..0x0109b000 are marked as code/data addresses to symbolize.
"""
import argparse
from pathlib import Path
import re
import struct
import sys

CLHE = 0x434c4845
FIRMWARE = (0x01000000, 0x0109b000)


def from_text(text):
    data = bytearray()
    for match in re.finditer(r'AOP crash: ([0-9a-f]{8}): ((?:[0-9a-f]{2} ?){1,16})', text):
        offset = int(match.group(1), 16)
        if offset != len(data):
            break  # a later, separate dump
        data += bytes.fromhex(match.group(2).replace(' ', ''))
    return bytes(data)


def tag(fourcc):
    raw = struct.pack('>I', fourcc)
    return ''.join(chr(b) if 32 <= b < 127 else '.' for b in raw)


def printable(blob):
    text = blob.split(b'\0', 1)[0]
    return text.decode('ascii', 'replace') if text and all(32 <= b < 127 or b in (9, 10) for b in text) else None


def decode(data):
    lines, offset = [], 32
    if len(data) < 32 or struct.unpack_from('<I', data)[0] != CLHE:
        return [f'not a CLHE crashlog ({len(data)} bytes)'], False
    version, used, flags = struct.unpack_from('<III', data, 4)
    complete = used <= len(data)
    lines.append(f'header CLHE version=0x{version:x} used=0x{used:x} flags=0x{flags:x} '
                 f'available=0x{len(data):x}{"" if complete else " TRUNCATED"}')
    end = min(used, len(data))
    while offset + 16 <= end:
        fourcc, unknown, kind, size = struct.unpack_from('<IIII', data, offset)
        lines.append(f'section off=0x{offset:x} tag={tag(fourcc)} size=0x{size:x} '
                     f'field4=0x{unknown:08x} field8=0x{kind:x}')
        if size < 16:
            lines.append('  invalid section size; stop')
            break
        body = data[offset + 16:min(offset + size, end)]
        if offset + size > end:
            lines.append(f'  section truncated: 0x{len(body):x} of 0x{size - 16:x} data bytes present')
        name = tag(fourcc)
        if name == 'Cver' and len(body) >= 16:
            lines.append(f'  uuid={body[:16].hex()} version={printable(body[16:])!r}')
        elif printable(body):
            lines.append(f'  string={printable(body)!r}')
        else:
            for i in range(0, len(body) - len(body) % 4, 4):
                word = struct.unpack_from('<I', body, i)[0]
                mark = '  <- firmware address' if FIRMWARE[0] <= word < FIRMWARE[1] else ''
                lines.append(f'  +0x{i:03x} [{i // 4:3d}] 0x{word:08x}{mark}')
        offset += size
    return lines, complete


def self_test():
    def section(name, kind, body):
        return struct.pack('<IIII', int.from_bytes(name.encode(), 'big'), 0xefefefef, kind, 16 + len(body)) + body
    body = (section('Cver', 0x200, bytes(range(16)) + b'RTKit: RTKit-2062.142.1.release\0\0\0\0') +
            section('Ctim', 0x100, struct.pack('<Q', 0x68d0ea684) + bytes(8)) +
            section('CrgA', 0x400, struct.pack('<8I', 4, 0x160, 0xc1100280, 0x010427f8, 0x200029, 0, 0x01046f98, 0x4040)))
    data = struct.pack('<IIII', CLHE, 0x301, 32 + len(body), 4) + bytes(16) + body
    lines, complete = decode(data)
    text = '\n'.join(lines)
    assert complete and "version='RTKit: RTKit-2062.142.1.release'" in text
    assert 'tag=CrgA' in text and '0x010427f8  <- firmware address' in text
    dump = ''.join(f'[ 1.0] AOP crash: {i:08x}: ' + ' '.join(f'{b:02x}' for b in data[i:i + 16]) + '\n'
                   for i in range(0, len(data), 16))
    assert from_text(dump) == data
    lines, complete = decode(data[:0x60])
    assert not complete and 'TRUNCATED' in lines[0]
    print('PASS crashlog decoder: CLHE header, Cver string, Ctim/CrgA words with firmware-address marks, dmesg round trip, truncation flagged')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', nargs='?', type=Path)
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.path:
        ap.error('path is required')
    raw = args.path.read_bytes()
    data = raw if raw[:4] == struct.pack('<I', CLHE) else from_text(raw.decode('utf-8', 'replace'))
    lines, complete = decode(data)
    print('\n'.join(lines))
    if not complete:
        print('INCOMPLETE: the source holds only part of the crashlog; use debugfs crashlog.bin', file=sys.stderr)


if __name__ == '__main__':
    main()
