#!/usr/bin/env python3
"""Clone an immutable local boot entry, replacing only its pstore reader.

Preserve the exact kernel, DTB, /init, firmware and rescue hooks. Does not
access the iPad. Upload the new entry separately; never overwrite the source.
"""
import argparse
import gzip
import hashlib
import importlib.machinery
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
merge = importlib.machinery.SourceFileLoader('merge', str(ROOT / 'scripts/merge-initramfs.py')).load_module()


def contents(data):
    return {merge.norm(n): (h, b) for n, h, b in merge.entries(data) if n != 'TRAILER!!!'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('out', type=Path)
    p.add_argument('--name', required=True)
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', a.name):
        p.error('invalid boot entry name')
    if a.out.exists():
        p.error('output already exists; choose a new path')
    if not any((a.source / k).is_file() for k in ('Image', 'Image.gz')):
        p.error('source needs Image or Image.gz')
    for f in ('dtb', 'cmdline', 'initramfs.gz'):
        if not (a.source / f).is_file():
            p.error('source is missing ' + f)
    manifest = a.source / 'SHA256SUMS'
    if manifest.exists():
        for line in manifest.read_text().splitlines():
            digest, name = line.split(maxsplit=1)
            name = name.lstrip('*').removeprefix('./')
            if '/' in name or hashlib.sha256((a.source / name).read_bytes()).hexdigest() != digest:
                p.error('source checksum mismatch: ' + name)
    base = gzip.decompress((a.source / 'initramfs.gz').read_bytes())
    files = contents(base)
    name = 'linux-a9-pstore'
    if name not in files or 'linux-a9-early-storage.sh' not in files:
        p.error('source lacks the early-storage pstore overlay')
    helper = (ROOT / 'scripts/boot/device' / name).read_bytes()
    header = files[name][0]
    header = header[:54] + b'%08X' % len(helper) + header[62:]
    overlay = merge.newc(name, header, helper, 0x7a9d001)
    overlay += merge.newc('TRAILER!!!', b'070701' + b'0' * 104, b'', 0)
    overlay += b'\0' * (-len(overlay) % 512)
    result = base + overlay
    revised = contents(result)
    assert set(files) == set(revised)
    assert all(revised[n][1] == b for n, (_, b) in files.items() if n != name)
    assert revised[name][1] == helper
    shutil.copytree(a.source, a.out)
    (a.out / 'initramfs.gz').write_bytes(gzip.compress(result, mtime=0))
    words = (a.out / 'cmdline').read_text().split()
    words = [w for w in words if not w.startswith('linux_a9.entry=')]
    (a.out / 'cmdline').write_text(' '.join(words + ['linux_a9.entry=' + a.name]) + '\n')
    (a.out / 'SOURCE').write_text('Pstore no-replay overlay of ' + str(a.source.resolve()) + '\n')
    sums = ''.join(hashlib.sha256(f.read_bytes()).hexdigest() + '  ./' + f.name + '\n'
                   for f in sorted(a.out.iterdir()) if f.is_file() and f.name != 'SHA256SUMS')
    (a.out / 'SHA256SUMS').write_text(sums)
    print('PASS only pstore reader and entry identity changed:', a.out)


if __name__ == '__main__':
    main()
