#!/usr/bin/env python3
"""Inspect local AOP firmware/captured windows; never contacts a device or authorizes attach."""
import argparse
import hashlib
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[3]
IMAGE = ROOT / 'build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin'
PINNED = 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8'


def describe(image):
    """Derive v4/v5 UUID, version-string and TLV offsets from file metadata."""
    headers = []
    for match in re.finditer(b'uuid', image):
        at = match.start()
        if at + 0x40 > len(image):
            continue
        version, rtkit, build = struct.unpack_from('<III', image, at + 4)
        if version not in (4, 5):
            continue
        field = at + (0x28 if version == 5 else 0x20)
        table, length = struct.unpack_from('<II', image, field)
        if table > len(image) or length > len(image) - table or length > 0x1000:
            continue
        tags = []
        cursor, end = table, table + length
        while cursor < end:
            if end - cursor < 8:
                break
            name, size = struct.unpack_from('<II', image, cursor)
            if size > end - cursor - 8:
                break
            tags.append((name.to_bytes(4, 'big').decode('ascii', errors='backslashreplace'), size))
            cursor += 8 + size
        else:
            raw = image[build:build + 256] if build < len(image) else b''
            headers.append(dict(header=at, version=version, rtkit=rtkit, build_offset=build,
                                build=raw.split(b'\0', 1)[0].decode('ascii', errors='backslashreplace'),
                                uuid=image[at + 0x10:at + 0x20].hex(), table=table,
                                table_size=length, tags=tags))
    return headers


def captured_chunks(text):
    """Yield explicit byte chunks; never infer bytes from diff counters."""
    current = None
    for line in text.splitlines():
        if line.startswith('== '):
            current = None
        if line.startswith('window='):
            found = re.search(r'region_offset=(0x[0-9a-f]+)', line)
            current = int(found[1], 16) if found else None
        old = re.match(r'^(head|build@47440) ([0-9a-f]{8}): ((?:[0-9a-f]{2} ){0,15}[0-9a-f]{2})(?:\s|$)', line)
        new = re.match(r'^actual ([0-9a-f]{8}): ((?:[0-9a-f]{2} ){0,15}[0-9a-f]{2})(?:\s|$)', line)
        if old:
            base = 0 if old[1] == 'head' else 0x47440
            offset, data = int(old[2], 16), bytes.fromhex(old[3])
        elif new and current is not None:
            base, offset, data = current, int(new[1], 16), bytes.fromhex(new[2])
        else:
            continue
        yield base, offset, data


def captured_windows(text):
    """Accept old patchbay snapshots and the new frozen fingerprint hexdumps."""
    windows = {}
    for base, offset, data in captured_chunks(text):
        chunks = windows.setdefault(base, {})
        chunks[offset] = data
    result = {}
    for base, chunks in windows.items():
        data = bytearray()
        while len(data) in chunks:
            data.extend(chunks[len(data)])
        result[base] = bytes(data)
    return result


def capture_evidence(text):
    """Count actual bytes present; a driver diff counter is not a hexdump.

    Merge overlapping prefix/header windows only when the bytes agree. This
    also detects a mixed capture rather than silently blessing the last copy.
    There is no address discovery or device operation in this host helper.
    """
    observed, conflicts = {}, set()
    for base, offset, data in captured_chunks(text):
        for index, value in enumerate(data, base + offset):
            if index in observed and observed[index] != value:
                conflicts.add(index)
            observed[index] = value
    windows = []
    for name, base, size in (('prefix', 0, 0x100), ('uuid', 0x20, 0x40),
                             ('build', 0x47440, 0x40), ('tags', 0x389b8, 0x1df)):
        available = sum(index in observed for index in range(base, base + size))
        windows.append(dict(name=name, base=base, size=size, available=available,
                            complete=available == size and not any(
                                index in conflicts for index in range(base, base + size))))
    return dict(windows=windows, conflicting_bytes=len(conflicts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=IMAGE)
    parser.add_argument('--capture', type=Path, help='saved active-patchbay.txt or active-fingerprint.txt')
    args = parser.parse_args()
    image = args.image.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    print(f'image={args.image}\nbytes={len(image)} sha256={digest} pinned_20H350={digest == PINNED}')
    for header in describe(image):
        print(f'uuid_header=0x{header["header"]:x} version={header["version"]} uuid_bytes={header["uuid"]}')
        print(f'build_offset=0x{header["build_offset"]:x} build={header["build"]!r}')
        print(f'tag_offset=0x{header["table"]:x} tag_bytes=0x{header["table_size"]:x} tags={len(header["tags"])}')
    if args.capture:
        captured = args.capture.read_text()
        for offset, actual in captured_windows(captured).items():
            expected = image[offset:offset + len(actual)]
            first = next((i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]), None)
            print(f'capture_window=0x{offset:x} bytes={len(actual)} equal_at_expected_offset={actual == expected}'
                  f' first_mismatch={None if first is None else hex(offset + first)}'
                  f' exact_window_elsewhere_in_file={image.find(actual) if actual else None}')
        evidence = capture_evidence(captured)
        for window in evidence['windows']:
            print(f'capture_evidence={window["name"]} actual_bytes={window["available"]}/{window["size"]}'
                  f' complete={window["complete"]}')
        print(f'capture_conflicting_bytes={evidence["conflicting_bytes"]}')
        print('Missing dump bytes do not establish relocation, another build, or an AP-readable alternate address.')
    print('Identification only: a matching build string never bypasses image/tag/ext/ownership refusal rules.')


if __name__ == '__main__':
    main()
