#!/usr/bin/env bash
# The cold-boot payload for warm reboots (docs/boot/warm-reboot.md): the
# NVMe-root payload of scripts/nvme-install/mkpayload.sh nvme, but with
#   - the PSCI m1n1 (build/m1n1-boot/m1n1.bin, scripts/boot/build-m1n1.sh)
#     and the payload variable psci=1, so CPUs can be offlined for kexec;
#   - crashkernel=$CRASHKERNEL for kexec-on-panic;
#   - panic=0 apple_wdt.cold_restart=0: nothing in Linux resets the SoC
#     (which would boot iOS) unless `linux-a9-kexec ios` asks for it;
#   - linux_a9.entry=NAME, the /boot/linux-a9 entry holding this same kernel
#     (scripts/boot/stage-kernel.sh NAME), so the device knows its pristine dtb.
#
#   [TREE=bootinteg] scripts/boot/mkpayload.sh [extra bootargs...]
# NAME defaults to $TREE-<commit>; ENTRY=name overrides it.
# Output: build/boot/m1n1-linux-boot.bin
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="${TREE:-bootinteg}"
K="$ROOT/build/kernel-$TREE"
B="$ROOT/build/nvme-install"
CRASHKERNEL="${CRASHKERNEL:-256M}"
M1N1="${M1N1:-$ROOT/build/m1n1-boot/m1n1.bin}"
INITRD="${INITRD:-$ROOT/build/boot/initramfs-omarchy.gz}"
OUT="$ROOT/build/boot/m1n1-linux-boot.bin"

[ -f "$M1N1" ] || { echo "no $M1N1: run scripts/boot/build-m1n1.sh" >&2; exit 1; }
[ -f "$K/COMMIT" ] || { echo "no $K: build the kernel first" >&2; exit 1; }
[ -f "$INITRD" ] || { echo "no $INITRD: run scripts/boot/build-initramfs.sh" >&2; exit 1; }
[ -f "$B/layout.env" ] || { echo "no $B/layout.env" >&2; exit 1; }
grep -qx 'CONFIG_KEXEC_FILE=y' "$K/config" ||
	echo "warning: $K has no CONFIG_KEXEC_FILE (integrate a9/boot by name)" >&2
grep -qx 'CONFIG_NVME_PCI_WRITE_WINDOW=y' "$K/config" ||
	{ echo "$K has no CONFIG_NVME_PCI_WRITE_WINDOW" >&2; exit 1; }
ww=$(grep '^WRITE_WINDOW=' "$B/layout.env" | cut -d= -f2)
[[ "$ww" =~ ^1:[0-9]+-[0-9]+$ ]] || { echo "bad WRITE_WINDOW '$ww' in layout.env" >&2; exit 1; }
ENTRY="${ENTRY:-$TREE-$(cut -c1-10 "$K/COMMIT")}"
[[ "$ENTRY" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo 'invalid ENTRY' >&2; exit 1; }

mkdir -p "$ROOT/build/boot"
PAYLOAD_OUT="$OUT" M1N1="$M1N1" M1N1_VARS="psci=1" TREE="$TREE" \
	"$ROOT/scripts/mkpayload.sh" "$INITRD" hid_apple.fnmode=2 "nvme.write_window=$ww" \
	omarchy.root=nvme panic=0 apple_wdt.cold_restart=0 crash_kexec_post_notifiers "crashkernel=$CRASHKERNEL" \
	"linux_a9.entry=$ENTRY" "$@"
echo "entry: $ENTRY (stage it: scripts/boot/stage-kernel.sh $ENTRY, TREE=$TREE)"
