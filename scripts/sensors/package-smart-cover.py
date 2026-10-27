#!/usr/bin/env python3
"""Stage the Smart Cover wake entry, offline (docs/power/smart-cover-test-plan.md).

Usage: package-smart-cover.py [--template DIR] [--name NAME]

Takes Image.gz and s8003-j71t-smart-cover.dtb from the integration build
(build/kernel-sensors, made by scripts/sensors/build-smart-cover.sh) and the
command line and initramfs of a known-good entry (default: the s2idle round-1
entry), replacing its linux_a9.entry=. Writes build/power/smart-cover-r1/entry/
with SHA256SUMS, and entry-name.txt next to it. Never touches the device.
"""
import argparse
import gzip
import hashlib
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/power/smart-cover-r1'
KERNEL = ROOT / 'build/kernel-sensors'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--template', type=Path,
                        default=ROOT / 'build/power/s2idle-round1/entry')
    parser.add_argument('--name')
    args = parser.parse_args()

    release = (KERNEL / 'RELEASE').read_text().strip()
    assert '-a9-smart-cover' in release, release
    assert (KERNEL / 'SOURCE.patch').read_bytes() == (OUT / 'integration.patch').read_bytes()
    # build-smart-cover.sh check must have passed on this merge.
    checks = OUT / 'integration'
    assert (checks / 'mock-tests.log').read_text().count('PASS') == 11
    assert (checks / 'dtb-properties.log').read_text().count('PASS') == 2
    assert (checks / 'validate-new.log').read_text() == ''
    image = gzip.decompress((KERNEL / 'Image.gz').read_bytes())
    assert image[56:60] == b'ARM\x64'
    dtb = KERNEL / 'dtbs/s8003-j71t-smart-cover.dtb'
    assert dtb.read_bytes()[:4] == b'\xd0\x0d\xfe\xed'

    owner = (OUT / 'owner-tip').read_text().strip()
    base = (OUT / 'integration-base').read_text().strip()
    name = args.name or 'smart-cover-r1-' + owner[:10]
    cmdline = [x for x in (args.template / 'cmdline').read_text().split()
               if not x.startswith('linux_a9.entry=')]
    cmdline.append('linux_a9.entry=' + name)
    if 'no_console_suspend' not in cmdline:
        cmdline.append('no_console_suspend')

    entry = OUT / 'entry'
    shutil.rmtree(entry, ignore_errors=True)
    entry.mkdir(parents=True)
    shutil.copyfile(KERNEL / 'Image.gz', entry / 'Image.gz')
    shutil.copyfile(dtb, entry / 'dtb')
    shutil.copyfile(args.template / 'initramfs.gz', entry / 'initramfs.gz')
    (entry / 'cmdline').write_text(' '.join(cmdline) + '\n')
    (entry / 'COMMIT').write_text(f'{owner} a9/sensors over a9/nextint {base}\n{release}\n')
    (entry / 'SOURCE').write_text(f'Detached base {base}\n'
                                  f'Overlay SHA256 {sha(OUT / "integration.patch")}\n'
                                  f'Template {args.template}\n')
    files = sorted(p for p in entry.iterdir() if p.is_file())
    (entry / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  ./{p.name}\n' for p in files))
    (OUT / 'entry-name.txt').write_text(name + '\n')
    print(f'PASS entry {name}: {release}, dtb s8003-j71t-smart-cover, '
          f'initramfs/cmdline from {args.template}')


if __name__ == '__main__':
    main()
