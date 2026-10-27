#!/usr/bin/env bash
# Offline only. Isolated branch a9/usb-host; never edits an integration tree.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
scripts/build-kernel.sh --run usb-host '
set -euo pipefail
test "$(git branch --show-current)" = a9/usb-host
test -z "$(git status --porcelain)"
base=d217ee93ec
if ! git merge-base --is-ancestor "$base" HEAD; then
    git merge --ff-only "$base"
fi
git merge --no-edit a9/usbhost
# tri52g has cherry-picks a8b7681bdb/a003e82cad of these two commits.
# Record the original lineage too; git merge makes no content change.
git merge --no-edit a412bc8f04
python3 -B /work/scripts/periph/prepare-usb-host.py "$PWD"
git add drivers/phy/apple/ausb.c drivers/power/supply/apple_tristar_power.c \
    drivers/usb/host/ehci-platform.c drivers/usb/host/ohci-platform.c \
    arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi \
    Documentation/devicetree/bindings/power/supply/apple,j71t-cbtl1610-power.yaml \
    arch/arm64/configs/apple_a9_usb_host.config
if ! git diff --cached --quiet; then
    git commit -s -m "usb: apple: integrate guarded J71t host storage and HID support" \
        -m "Keep charger ADC routing away from the shared host PHY. Retain digital
device-mode current grants, enforce the ADT 100 mA host budget, and build
storage, filesystems and HID into the trial kernel."
fi
if [ ! -f .config ]; then cp /work/build/kernel-tri52g/config .config; fi
scripts/kconfig/merge_config.sh -m .config /work/patches/usbhost/morning.config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
python3 - "$PWD/.config" /work/patches/usbhost/morning.config <<"PY"
from pathlib import Path
import sys
config = set(Path(sys.argv[1]).read_text().splitlines())
for line in Path(sys.argv[2]).read_text().splitlines():
    if line.startswith("CONFIG_") and line not in config:
        raise SystemExit("Kconfig dropped " + line)
PY
mkdir -p /work/build/usb-host
git format-patch --no-signature -1 -o /work/patches/usbhost
git log -1 --format="%H %s" > /work/build/usb-host/branch.txt
git merge-base --is-ancestor a412bc8f04 HEAD
git merge-base --is-ancestor 07377c65b6 HEAD
'
KERNEL_JOBS="${KERNEL_JOBS:-6}" scripts/build-kernel.sh --tree usb-host
python3 -B -I scripts/periph/package-usb-host.py
