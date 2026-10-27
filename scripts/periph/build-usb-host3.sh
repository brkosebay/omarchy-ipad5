#!/usr/bin/env bash
# Offline only. Branch a9/usb-host3 = a9/usb-host2 9977e33ee6 + the CBTL1610
# ID-bus TX window fix (0x5d..0x61, read-only RX at 0x20), reorient RX
# acceptance and debugfs link recovery. Builds the committed tree as-is;
# never edits it. Cleans the tree's intermediates afterwards (the volume is
# small); .config stays.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
scripts/build-kernel.sh --run usb-host3 '
set -euo pipefail
test "$(git branch --show-current)" = a9/usb-host3
test -z "$(git status --porcelain)"
for base in 9977e33ee6 0cfbadcd4f a412bc8f04 d217ee93ec 07377c65b6; do
    git merge-base --is-ancestor "$base" HEAD
done
# A fresh worktree would get hoolock-16k.config and lose the host options.
if [ ! -f .config ]; then cp /work/build/kernel-usb-host2/config .config; fi
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
python3 - "$PWD/.config" /work/patches/usbhost/morning.config <<"PY"
from pathlib import Path
import sys
config = set(Path(sys.argv[1]).read_text().splitlines())
for line in Path(sys.argv[2]).read_text().splitlines():
    if line.startswith("CONFIG_") and line not in config:
        raise SystemExit("Kconfig dropped " + line)
for line in ("CONFIG_DEBUG_FS=y", "CONFIG_APPLE_D2365_CHARGER=y"):
    if line not in config:
        raise SystemExit("missing " + line)
if "CONFIG_DEBUG_INFO=y" in config:
    raise SystemExit("debug info must stay off")
PY
mkdir -p /work/build/usb-host3 /work/patches/usbhost3
rm -f /work/patches/usbhost3/*.patch
git format-patch --no-signature 9977e33ee6..HEAD -o /work/patches/usbhost3
git log -1 --format="%H %s" > /work/build/usb-host3/branch.txt
'
KERNEL_JOBS="${KERNEL_JOBS:-6}" scripts/build-kernel.sh --tree usb-host3
python3 -B -I scripts/periph/package-usb-host3.py
# Intermediates are ~1.5 GB in a nearly full volume; the outputs are in
# build/kernel-usb-host3. make clean keeps .config.
scripts/build-kernel.sh --run usb-host3 'make LLVM=1 ARCH=arm64 clean >/dev/null; df -h /src | tail -1'
