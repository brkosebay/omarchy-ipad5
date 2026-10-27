#!/usr/bin/env python3
"""Verify the clean touch source, linked/staged drivers, and artifact hashes."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='compare existing manifest, do not update it')
    args = parser.parse_args()
    command = r'''
set -euo pipefail
test -z "$(git status --porcelain)"
test ! -s /work/build/kernel-touch/DIRTY
test "$(cut -d ' ' -f1 /work/build/kernel-touch/COMMIT)" = "$(git rev-parse HEAD)"
cmp arch/arm64/boot/Image.gz /work/build/kernel-touch/Image.gz
cmp arch/arm64/boot/dts/apple/s8003-j71t.dtb /work/build/kernel-touch/dtbs/s8003-j71t.dtb
symbols=$(llvm-nm vmlinux)
for symbol in n1_probe s8000_spi_probe s8000_touch_clock_probe d2365_touch_probe; do
    grep -Eq " $symbol$" <<<"$symbols"
done
for symbol in INPUT_TOUCHSCREEN INPUT_EVDEV TOUCHSCREEN_APPLE_N1 REGULATOR \
    REGULATOR_APPLE_D2365_TOUCH SPI_APPLE_S8000 COMMON_CLK_APPLE_S8000_TOUCH DEBUG_INFO_NONE; do
    grep -qx "CONFIG_${symbol}=y" .config
done
# The fragment is consumed independently by integration; validate every
# requested built-in option as well as the standalone driver's checks.
while IFS= read -r option; do
    case "$option" in
        CONFIG_*=y) grep -qxF "$option" .config ;;
    esac
done </work/patches/touch/touch.config
dtb=/work/build/kernel-touch/dtbs/s8003-j71t.dtb
node=/soc/spi@20a08c000/touchscreen@0
test "$(fdtget -t s "$dtb" "$node" compatible)" = apple,j71t-n1
test "$(fdtget -t x "$dtb" "$node" interrupts)" = '41 2'
test "$(fdtget -t s "$dtb" "$node" firmware-name)" = apple/j71t-n1.bin
test "$(fdtget -t s "$dtb" "$node" apple,calibration-firmware)" = apple/j71t-n1-calibration.bin
test "$(fdtget -t u "$dtb" "$node" spi-cs-setup-delay-ns)" = 5000
test "$(fdtget -t u "$dtb" "$node" spi-cs-hold-delay-ns)" = 10000
fdtget -p "$dtb" "$node" | grep -qx touchscreen-inverted-y
! fdtget -p "$dtb" "$node" | grep -Eq '^touchscreen-(inverted-x|swapped-x-y)$'
grep -q 'apple/j71t-n1.bin' modules.builtin.modinfo
grep -q 'apple/j71t-n1-calibration.bin' modules.builtin.modinfo
echo 'PASS: clean source, all four drivers linked, built-in config and J71t DT verified'
'''
    subprocess.run([str(ROOT / 'scripts/build-kernel.sh'), '--run', 'touch', command], check=True)
    subprocess.run([sys.executable, '-B', str(ROOT / 'scripts/touch/mkinitramfs.py'), '--check'], check=True)
    files = ['build/kernel-touch/Image.gz', 'build/kernel-touch/dtbs/s8003-j71t.dtb',
             'build/touch/initramfs-touch.gz', 'build/touch/device/n1-input-info']
    kernel = gzip.decompress((ROOT / files[0]).read_bytes())
    if kernel[56:60] != b'ARMd':
        raise ValueError('not an arm64 Linux Image')
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    stream.decompress((ROOT / files[2]).read_bytes())
    stream.flush()
    if not stream.eof or stream.unused_data:
        raise ValueError('initramfs is not a single complete gzip member')
    helper = (ROOT / files[3]).read_bytes()
    if helper[:5] != b'\x7fELF\x02' or int.from_bytes(helper[18:20], 'little') != 183:
        raise ValueError('input-info helper is not an arm64 ELF')
    report = {'kernel': (ROOT / 'build/kernel-touch/COMMIT').read_text().strip(),
              'source_clean': not (ROOT / 'build/kernel-touch/DIRTY').read_bytes(), 'artifacts': []}
    for name in files:
        data = (ROOT / name).read_bytes()
        report['artifacts'].append({'path': name, 'size': len(data),
                                    'sha256': hashlib.sha256(data).hexdigest()})
    path = ROOT / 'build/touch/artifacts.json'
    if args.check:
        if json.loads(path.read_text()) != report:
            raise ValueError('artifact manifest differs; review the new build before updating')
    else:
        path.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: artifact sizes/hashes and single-stream initramfs; build/touch/artifacts.json')


if __name__ == '__main__':
    main()
