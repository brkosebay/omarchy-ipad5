#!/usr/bin/env bash
# The m1n1 payloads for the NVMe install, from layout.env:
#
#   scripts/nvme-install/mkpayload.sh ro       # read-only kernel args, NBD root (backup)
#   scripts/nvme-install/mkpayload.sh window   # NBD root, p2 writable (soak, install)
#   scripts/nvme-install/mkpayload.sh nvme     # root on p2, NBD as fallback
#
# Kernel: build/kernel-$TREE (TREE=integ by default), which must have
# CONFIG_NVME_PCI_WRITE_WINDOW=y for window/nvme. m1n1: the patched one
# (NVMe HMB), build/m1n1-pcie/m1n1.bin. Initramfs:
# build/nvme-install/initramfs-omarchy.gz (scripts/nvme-install/build-initramfs.sh).
# Extra bootargs after the mode are passed on (e.g. omarchy.rescue).
# Output: build/nvme-install/m1n1-linux-MODE.bin
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/build/nvme-install"
MODE="${1:?ro|window|nvme}"
shift
TREE="${TREE:-integ}"
K="$ROOT/build/kernel-$TREE"
INITRD="$B/initramfs-omarchy.gz"
[ -f "$INITRD" ] || "$ROOT/scripts/nvme-install/build-initramfs.sh"
args=(hid_apple.fnmode=2)
case "$MODE" in
ro) ;;
window | nvme)
	[ -f "$B/layout.env" ] || { echo "no $B/layout.env" >&2; exit 1; }
	grep -qx 'CONFIG_NVME_PCI_WRITE_WINDOW=y' "$K/config" ||
		{ echo "$K has no CONFIG_NVME_PCI_WRITE_WINDOW: integrate a9/nvme-install and its fragment first" >&2; exit 1; }
	ww=$(grep '^WRITE_WINDOW=' "$B/layout.env" | cut -d= -f2)
	[[ "$ww" =~ ^1:[0-9]+-[0-9]+$ ]] || { echo "bad WRITE_WINDOW '$ww' in layout.env" >&2; exit 1; }
	args+=("nvme.write_window=$ww")
	[ "$MODE" = nvme ] && args+=(omarchy.root=nvme)
	;;
*) echo "unknown mode $MODE" >&2; exit 1 ;;
esac
TREE="$TREE" M1N1="${M1N1:-$ROOT/build/m1n1-pcie/m1n1.bin}" "$ROOT/scripts/mkpayload.sh" "$INITRD" "${args[@]}" "$@"
cp "$ROOT/build/m1n1-linux.bin" "$B/m1n1-linux-$MODE.bin"
echo "payload: $B/m1n1-linux-$MODE.bin (kernel $(cut -c1-12 "$K/COMMIT"))"
