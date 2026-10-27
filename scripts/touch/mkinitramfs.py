#!/usr/bin/env python3
"""Offline HoolockRD + N1 firmware/calibration, preserving the base archive."""
import argparse
import gzip
import hashlib
import importlib.util
import json
import lzma
import os
from pathlib import Path
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('periph_initramfs', ROOT / 'scripts/periph/mkinitramfs.py')
periph = importlib.util.module_from_spec(spec)
spec.loader.exec_module(periph)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    base = periph.BASE.read_bytes()
    count, entries = periph.read_newc(gzip.decompress(base))
    original_files = {name: body for _, name, _, body, _ in entries}
    if count != 1 or not original_files.get('init') or 'extract.cpio.lzma' not in original_files:
        raise ValueError('not the expected two-stage HoolockRD base')
    _, nested = periph.read_newc(lzma.decompress(original_files['extract.cpio.lzma']))
    staged = {}
    for name, length in [('apple/j71t-n1.bin', 57016), ('apple/j71t-n1-calibration.bin', 1024)]:
        data = (ROOT / 'build/firmware/lib/firmware' / name).read_bytes()
        if len(data) != length:
            raise ValueError('run scripts/touch/stage-firmware.py first')
        staged['lib/firmware/' + name] = data
    if set(staged) & (set(original_files) | {name for _, name, _, _, _ in nested}):
        raise ValueError('HoolockRD overwrites a touch firmware path')
    modes = {name: 0o644 for name in staged}
    out = ROOT / 'build/touch/initramfs-touch.gz'
    # m1n1 replaces initrd for EACH gzip member. Concatenate the newc
    # archives inside ONE stream, so the firmware survives handoff.
    combined = out.read_bytes() if args.check else gzip.compress(
        gzip.decompress(base) + periph.overlay(staged, modes),
        compresslevel=9, mtime=0)
    inflater = zlib.decompressobj(16 + zlib.MAX_WBITS)
    payload = inflater.decompress(combined) + inflater.flush()
    if not inflater.eof or inflater.unused_data:
        raise ValueError('touch initramfs must contain exactly one gzip member')
    original = gzip.decompress(base)
    if not payload.startswith(original):
        raise ValueError('original HoolockRD newc bytes were modified')
    periph.verify(combined, base, staged, modes)
    if not args.check:
        os.umask(0o077)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(combined)
        out.chmod(0o600)
        report = {'output': str(out.relative_to(ROOT)), 'sha256': hashlib.sha256(combined).hexdigest(),
                  'base_sha256': hashlib.sha256(base).hexdigest(), 'size': len(combined),
                  'files': sorted(staged), 'gzip_members': 1, 'newc_archives': 2,
                  'note': 'one gzip stream, original HoolockRD newc followed by touch-only firmware newc'}
        (out.parent / 'initramfs-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {out.relative_to(ROOT)} ({len(combined)} bytes), checked all {len(staged)} staged files')


if __name__ == '__main__':
    main()
