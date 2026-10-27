#!/usr/bin/env bash
# Offline only. usb-host4 keeps usb-host3 hardware support and removes the
# legacy g_ffs gadget so configfs ffs.iap owns FunctionFS's device slot.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
scripts/build-kernel.sh --run usb-host4 '
set -euo pipefail
test "$(git branch --show-current)" = a9/usb-host4
test -z "$(git status --porcelain)"
git merge-base --is-ancestor f585351c82 HEAD
test "$(git rev-list --count f585351c82..HEAD)" = 1
if [ ! -f .config ]; then cp /work/build/kernel-usb-host3/config .config; fi
scripts/config -d USB_FUNCTIONFS -e USB_CONFIGFS_F_FS \
    -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
grep -qx "CONFIG_USB_CONFIGFS_F_FS=y" .config
grep -qx "# CONFIG_USB_FUNCTIONFS is not set" .config
grep -qx "CONFIG_USB_CONFIGFS_ACM=y" .config
grep -qx "CONFIG_USB_CONFIGFS_ECM=y" .config
grep -qx "CONFIG_DEBUG_INFO_NONE=y" .config
mkdir -p /work/build/usb-host4 /work/patches/usbhost4
rm -f /work/patches/usbhost4/*.patch
git format-patch --no-signature f585351c82..HEAD -o /work/patches/usbhost4
git log -1 --format="%H %s" > /work/build/usb-host4/branch.txt
'
KERNEL_JOBS="${KERNEL_JOBS:-6}" scripts/build-kernel.sh --tree usb-host4
python3 -B -I scripts/periph/package-usb-host4.py
scripts/build-kernel.sh --run usb-host4 'make LLVM=1 ARCH=arm64 clean >/dev/null; df -h /src | tail -1'
