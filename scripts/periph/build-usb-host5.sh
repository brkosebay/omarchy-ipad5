#!/usr/bin/env bash
# Offline only. usb-host5: the iOS mode-2 camera-adapter path (ds-usb-host5.md)
# on the validated irqfix-usb4 lineage: a9/usb-host4 + the a9/kexec-irqfix NVMe
# shared-MSI fix (a9/kexec-irqfix-usb4 209a5a55e3), plus four commits on
# a9/usb-host5. Built with irqfix-usb4's config, packaged as entry
# usb-host5-<hash>: irqfix-usb4's m1n1, initramfs, DTB and cmdline, with the
# kernel and entry name changed only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
scripts/build-kernel.sh --run usb-host5 '
set -euo pipefail
test "$(git branch --show-current)" = a9/usb-host5
test -z "$(git status --porcelain --untracked-files=no)"
test "$(git rev-parse --short=10 HEAD~4)" = 209a5a55e3
test "$(git rev-list --count 209a5a55e3..HEAD)" = 4
# a9/kexecfix is an ancestor; a9/kexec-irqfix (7dcb18c411, e3ad1b3a5c) is
# cherry-picked: both of its commits have a patch-identical equivalent.
git merge-base --is-ancestor a412bc8f04 HEAD
test "$(git cherry HEAD a9/kexec-irqfix a412bc8f04 | grep -c "^-")" = 2
test -z "$(git cherry HEAD a9/kexec-irqfix a412bc8f04 | grep "^+" || true)"
if [ ! -f .config ]; then cp /work/build/kernel-kexec-irqfix-usb4/config .config; fi
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
cmp .config /work/build/kernel-kexec-irqfix-usb4/config
grep -qx "CONFIG_DEBUG_INFO_NONE=y" .config
mkdir -p /work/build/usb-host5 /work/patches/usbhost5
rm -f /work/patches/usbhost5/*.patch
git format-patch --no-signature 209a5a55e3..HEAD -o /work/patches/usbhost5
git log -1 --format="%H %s" > /work/build/usb-host5/branch.txt
'
KERNEL_JOBS="${KERNEL_JOBS:-6}" scripts/build-kernel.sh --tree usb-host5
python3 -B -I scripts/periph/package-usb-host5.py
scripts/build-kernel.sh --run usb-host5 'make LLVM=1 ARCH=arm64 clean >/dev/null; df -h /src | tail -1'
