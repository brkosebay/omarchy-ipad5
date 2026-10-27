#!/usr/bin/env bash
# Offline validation for the configfs-only FunctionFS usb-host4 entry.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
TREE=usb-host4 scripts/periph/test-tristar-idbus.sh
python3 -B -I scripts/periph/test-usb-host.py
scripts/build-kernel.sh --run usb-host4 '
set -euo pipefail
test "$(git branch --show-current)" = a9/usb-host4
test -z "$(git status --porcelain)"
test "$(git rev-list --count f585351c82..HEAD)" = 1
test "$(git diff --name-only f585351c82 HEAD)" = \
    arch/arm64/configs/apple_a9_usb_host.config
grep -qx "CONFIG_USB_CONFIGFS_F_FS=y" .config
grep -qx "# CONFIG_USB_FUNCTIONFS is not set" .config
make -j1 LLVM=1 ARCH=arm64 W=1 \
    drivers/usb/dwc2/core.o \
    drivers/usb/dwc2/core_intr.o \
    drivers/usb/dwc2/platform.o \
    drivers/usb/dwc2/drd.o \
    drivers/usb/dwc2/params.o \
    drivers/usb/dwc2/hcd.o \
    drivers/usb/dwc2/hcd_intr.o \
    drivers/usb/dwc2/hcd_queue.o \
    drivers/usb/dwc2/hcd_ddma.o \
    drivers/usb/dwc2/gadget.o \
    drivers/usb/dwc2/debugfs.o
git format-patch --stdout -1 HEAD | ./scripts/checkpatch.pl --strict --no-tree \
    --ignore FILE_PATH_CHANGES -
make LLVM=1 ARCH=arm64 clean >/dev/null
'
python3 -B -I scripts/periph/package-usb-host4.py
echo "PASS usb-host4 offline checks"
