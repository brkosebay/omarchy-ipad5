#!/usr/bin/env python3
"""Assemble/check HoolockRD + firmware/tools in one gzip stream, two newc archives.

No extraction to the host filesystem, Docker, root privileges, or network.
Host output is private (0600); data is root-owned 0644, executable tools 0755.
"""
import argparse
import gzip
import hashlib
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/prebuilt/initramfs/initramfs.gz'
FIRMWARE = ROOT / 'build/firmware/lib/firmware'
OUTPUT = ROOT / 'build/initramfs-fw.gz'
REPORT = ROOT / 'build/periph/initramfs-manifest.json'
USERSPACE = ROOT / 'build/periph/rootfs'
USERSPACE_FILES = {'usr/bin/btmgmt': 0o755, 'usr/share/licenses/bluez/COPYING': 0o644}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def newc_entry(name, data, mode, inode):
    encoded = name.encode() + b'\0'
    fields = (inode, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, 0,
              len(data), 0, 0, 0, 0, len(encoded), 0)
    result = b'070701' + b''.join(f'{v:08x}'.encode() for v in fields) + encoded
    result += b'\0' * (-len(result) % 4)
    result += data
    return result + b'\0' * (-len(data) % 4)


def read_newc(data):
    """Parse concatenated archives, including padding after each trailer."""
    pos, archive, entries = 0, 0, []
    closed = False
    while pos < len(data):
        if data[pos] == 0:
            pos += 1
            continue
        if data[pos:pos + 6] not in (b'070701', b'070702') or pos + 110 > len(data):
            raise ValueError(f'invalid newc header at {pos}')
        fields = [int(data[pos + 6 + i * 8:pos + 14 + i * 8], 16) for i in range(13)]
        namesize, size = fields[11], fields[6]
        endname = pos + 110 + namesize
        start = (endname + 3) & ~3
        end = start + size
        if namesize < 1 or end > len(data) or data[endname - 1] != 0:
            raise ValueError('truncated newc record')
        name = data[pos + 110:endname - 1].decode()
        pos = (end + 3) & ~3
        if name == 'TRAILER!!!':
            archive += 1
            closed = True
            continue
        closed = False
        if name.startswith('./'):
            name = name[2:]
        if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts:
            raise ValueError('unsafe path in newc')
        entries.append((archive, name, fields[1], data[start:end], fields))
    if not closed:
        raise ValueError('missing newc trailer')
    return archive, entries


def inputs():
    base = BASE.read_bytes()
    count, entries = read_newc(gzip.decompress(base))
    if count != 1:
        raise ValueError('expected one original HoolockRD archive')
    files = {name: body for _, name, _, body, _ in entries}
    if not files.get('init') or 'extract.cpio.lzma' not in files:
        raise ValueError('not the expected two-stage HoolockRD initramfs')
    _, inner = read_newc(lzma.decompress(files['extract.cpio.lzma']))
    nested_names = {name for _, name, _, _, _ in inner}
    staged = {}
    for path in sorted(FIRMWARE.rglob('*')):
        if path.is_symlink():
            raise ValueError(f'firmware symlink not supported: {path}')
        if path.is_file():
            staged['lib/firmware/' + path.relative_to(FIRMWARE).as_posix()] = path.read_bytes()
    for name in ('brcm/BCM.apple,j71t.hcd', 'apple/j71t-LSCI.bin',
                 'apple/j71t-CBAT.bin', 'apple/j71t-BMac.bin'):
        if not staged.get('lib/firmware/' + name):
            raise ValueError(f'missing staged firmware: {name}; run offline pull first')
    modes = {name: 0o644 for name in staged}
    for name, mode in USERSPACE_FILES.items():
        path = USERSPACE / name
        if path.is_symlink():
            raise ValueError(f'userspace symlink not supported: {path}')
        if path.is_file():
            staged[name] = path.read_bytes()
            modes[name] = mode
    if 'usr/bin/btmgmt' in staged:
        manifest = json.loads((ROOT / 'build/periph/btmgmt-manifest.json').read_text())
        if sha(staged['usr/bin/btmgmt']) != manifest['sha256']:
            raise ValueError('btmgmt differs from checked static build')
        if 'usr/share/licenses/bluez/COPYING' not in staged:
            raise ValueError('missing BlueZ license')
        # The owner can use the actual unit identity for a RAM-only MGMT
        # public-addr test if the bootloader has left HCI unconfigured.
        mac = (ROOT / 'build/firmware/private/BMac.bin').read_bytes()
        if len(mac) != 6 or not any(mac) or mac == b'\xff' * 6 or mac[0] & 1:
            raise ValueError('invalid private Bluetooth identity')
        staged['etc/periph/BMac.bin'] = mac
        modes['etc/periph/BMac.bin'] = 0o600
    conflicts = set(staged) & (nested_names | set(files))
    if conflicts:
        raise ValueError(f'HoolockRD inner archive overwrites firmware: {sorted(conflicts)}')
    tools = sorted(n for n in set(files) | nested_names | set(staged)
                   if any(t in n for t in ('btmgmt', 'btmon', 'bluetoothctl', 'bluetoothd')))
    return base, staged, modes, tools


def overlay(staged, modes):
    directories = {'lib', 'lib/firmware'}
    for name in staged:
        directories.update(str(p) for p in PurePosixPath(name).parents if str(p) != '.')
    records = [(d, b'', stat.S_IFDIR | 0o755) for d in sorted(directories, key=lambda d: (d.count('/'), d))]
    records += [(name, data, stat.S_IFREG | modes[name]) for name, data in sorted(staged.items())]
    archive = b''.join(newc_entry(n, b, m, i + 1) for i, (n, b, m) in enumerate(records))
    archive += newc_entry('TRAILER!!!', b'', 0, len(records) + 1)
    archive += b'\0' * (-len(archive) % 512)
    return archive


def verify(combined, base, staged, modes):
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    raw = stream.decompress(combined) + stream.flush()
    if not stream.eof or stream.unused_data:
        raise ValueError('expected exactly one complete gzip member for m1n1')
    original = gzip.decompress(base)
    if not raw.startswith(original) or raw == original:
        raise ValueError('original HoolockRD cpio is not preserved as the exact prefix')
    count, entries = read_newc(raw)
    if count != 2:
        raise ValueError(f'expected two concatenated newc archives, got {count}')
    appended = {name: body for a, name, mode, body, _ in entries if a == 1 and stat.S_ISREG(mode)}
    if appended != staged:
        raise ValueError('combined initramfs does not match the staged firmware')
    for a, name, mode, body, fields in entries:
        if a != 1:
            continue
        if fields[2:4] != [0, 0] or fields[5] != 0:
            raise ValueError(f'non-deterministic ownership/time: {name}')
        expected = 0o755 if stat.S_ISDIR(mode) else modes[name]
        if stat.S_IMODE(mode) != expected:
            raise ValueError(f'incorrect archive permissions: {name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='verify existing output; do not write')
    parser.add_argument('--require-btmgmt', action='store_true', help='fail unless the checked static tool is packaged')
    args = parser.parse_args()
    base, staged, modes, tools = inputs()
    if args.require_btmgmt and 'usr/bin/btmgmt' not in staged:
        raise ValueError('missing static btmgmt; run scripts/periph/build-btmgmt.sh')
    os.umask(0o077)
    if args.check:
        combined = OUTPUT.read_bytes()
        verify(combined, base, staged, modes)
    else:
        # m1n1 replaces the initrd for EACH gzip payload member. Both cpio
        # archives must therefore share one stream (gzip -dc | gzip -9n).
        combined = gzip.compress(gzip.decompress(base) + overlay(staged, modes),
                                 compresslevel=9, mtime=0)
        verify(combined, base, staged, modes)
        temp = ROOT / 'build/periph/initramfs-fw.gz.tmp'
        temp.parent.mkdir(parents=True, exist_ok=True)
        temp.write_bytes(combined)
        temp.chmod(0o600)
        temp.replace(OUTPUT)
        manifest = {'base': str(BASE.relative_to(ROOT)), 'base_sha256': sha(base),
                    'output': str(OUTPUT.relative_to(ROOT)), 'size': len(combined),
                    'sha256': sha(combined), 'gzip_members': 1, 'newc_archives': 2,
                    'bluez_tools': tools, 'firmware': [
                        {'path': name, 'size': len(data), 'sha256': sha(data)}
                        for name, data in sorted(staged.items()) if name.startswith('lib/firmware/')],
                    'userspace': [
                        {'path': name, 'mode': oct(modes[name]), 'size': len(data), 'sha256': sha(data)}
                        for name, data in sorted(staged.items()) if not name.startswith('lib/firmware/')]}
        REPORT.write_text(json.dumps(manifest, indent=2) + '\n')
        REPORT.chmod(0o600)
    print(f'PASS: {OUTPUT.relative_to(ROOT)} ({len(combined)} bytes), {len(staged)} overlay files, one gzip stream / two newc archives')
    print('SHA256 ' + sha(combined))
    print('BlueZ tools: ' + (', '.join(tools) if tools else 'absent; first boot supports kernel/sysfs probes only'))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, EOFError, lzma.LZMAError) as error:
        sys.exit(str(error))
