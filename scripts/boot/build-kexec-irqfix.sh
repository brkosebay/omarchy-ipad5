#!/usr/bin/env bash
# Offline only. The NVMe shared-MSI fix (a9/kexec-irqfix = a9/kexecfix + 2
# commits) cherry-picked onto a9/usb-host4 as a9/kexec-irqfix-usb4, built with
# usb-host4's config and packaged as entry irqfix-usb4-<hash>: usb-host4's
# m1n1, initramfs, DTB and cmdline, kernel and entry name changed only.
# Root cause and device plan: docs/boot/ds-kexec-nvme-crawl.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
scripts/build-kernel.sh --run kexecfix '
set -euo pipefail
cd /src/linux
test "$(git rev-list --count a412bc8f04..a9/kexec-irqfix)" = 2
test "$(git rev-parse --short=10 a9/usb-host4)" = 09e836312a
if ! git rev-parse -q --verify refs/heads/a9/kexec-irqfix-usb4 >/dev/null; then
	git worktree prune
	git worktree add -b a9/kexec-irqfix-usb4 /src/wt/kexec-irqfix-usb4 a9/usb-host4
	git -C /src/wt/kexec-irqfix-usb4 cherry-pick a412bc8f04..a9/kexec-irqfix
fi
test "$(git rev-list --count a9/usb-host4..a9/kexec-irqfix-usb4)" = 2
# Same change on both bases: identical NVMe diff, nothing else touched.
cmp <(git diff a9/kexecfix a9/kexec-irqfix) \
    <(git diff a9/usb-host4 a9/kexec-irqfix-usb4)
'
scripts/build-kernel.sh --run kexec-irqfix-usb4 '
set -euo pipefail
test "$(git branch --show-current)" = a9/kexec-irqfix-usb4
test -z "$(git status --porcelain)"
if [ ! -f .config ]; then cp /work/build/kernel-usb-host4/config .config; fi
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
cmp .config /work/build/kernel-usb-host4/config
mkdir -p /work/build/kexec-irqfix /work/patches/kexec-irqfix
rm -f /work/patches/kexec-irqfix/*.patch
git format-patch --no-signature a9/usb-host4..HEAD -o /work/patches/kexec-irqfix
git log -1 --format="%H %s" > /work/build/kexec-irqfix/branch.txt
'
KERNEL_JOBS="${KERNEL_JOBS:-6}" scripts/build-kernel.sh --tree kexec-irqfix-usb4
python3 -B -I scripts/boot/package-kexec-irqfix.py
scripts/build-kernel.sh --run kexec-irqfix-usb4 'make LLVM=1 ARCH=arm64 clean >/dev/null; df -h /src | tail -1'
