#!/usr/bin/env bash
# Offline only. Branch a9/usb-host2 = a9/usb-host 0cfbadcd4f + the manual
# Tristar ID-bus identify. Builds the committed tree as-is; never edits it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
scripts/build-kernel.sh --run usb-host2 '
set -euo pipefail
test "$(git branch --show-current)" = a9/usb-host2
test -z "$(git status --porcelain)"
for base in 0cfbadcd4f a412bc8f04 d217ee93ec 07377c65b6; do
    git merge-base --is-ancestor "$base" HEAD
done
# A fresh worktree would get hoolock-16k.config and lose the host options.
if [ ! -f .config ]; then cp /work/build/kernel-usb-host/config .config; fi
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
mkdir -p /work/build/usb-host2 /work/patches/usbhost2
rm -f /work/patches/usbhost2/*.patch
git format-patch --no-signature 0cfbadcd4f..HEAD -o /work/patches/usbhost2
git log -1 --format="%H %s" > /work/build/usb-host2/branch.txt
'
KERNEL_JOBS="${KERNEL_JOBS:-6}" scripts/build-kernel.sh --tree usb-host2
python3 -B -I scripts/periph/package-usb-host2.py
