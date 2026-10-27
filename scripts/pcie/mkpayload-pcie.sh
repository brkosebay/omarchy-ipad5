#!/usr/bin/env bash
# Build the pcie boot payloads from build/kernel-pcie (build it with
# scripts/pcie/build.sh). Refuses to run if the kernel config lacks the
# patches/pcie/pcie.config options.
#
# All payloads: patched m1n1, NVMe on port 0 (read-only, system namespaces
# write-protected), Wi-Fi on port 1 (brought up after probe), live-ADT
# tunables. The NVMe and Wi-Fi bring-ups run in the background, so the
# shell comes up whatever they do. Round 7 (see docs/pcie/test-plan.md):
#
#   r7        MAIN. NVMe + Wi-Fi (regdb in the initramfs, country to the
#             firmware, no P2P), bring-up diagnostics off, ASPM off.
#   r7-aspm   the same with pcie_apple_h9p.aspm_l1_ports=0x2: ASPM L1 on
#             the Wi-Fi link (power test; policy switchable at runtime).
#   r7-debug  r7 with nvme.apple_ans_debug=1 (host/NVMMU/DART dumps, DART
#             walks) and brcmfmac debug messages (firmware names, OTP, PCIe
#             bring-up).
# Country: pass cfg80211.ieee80211_regdom=<CC> as an extra bootarg (below).
#
# Patched m1n1 = build/m1n1-pcie/m1n1.bin (scripts/pcie/build-m1n1.sh): it
# copies iBoot's NVMe HMB (live ADT sart-region) and the Wi-Fi calibration
# (uart2/wlan wifi-calibration-msf) into the DT; stock m1n1 already sets the
# Wi-Fi MAC.
#
# Usage: scripts/pcie/mkpayload-pcie.sh [initramfs.gz] [extra bootargs...]
# The initramfs defaults to build/kernel-pcie/initramfs-wifi.gz (prebuilt
# HoolockRD + Wi-Fi firmware, scripts/pcie/mkinitramfs-wifi.sh); it is
# built if missing.
# Always adds: panic=0 (keep a panic on screen; the config panics/reboots
# otherwise) and nvme_core.admin_timeout=15 (a broken admin path fails in
# seconds per command instead of 60 s each).
# The shared build/m1n1-linux.bin is left holding the last payload built.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
K="$ROOT/build/kernel-pcie"
PATCHED="$ROOT/build/m1n1-pcie/m1n1.bin"

"$ROOT/scripts/pcie/check-config.sh" "$K/config"
echo "kernel: $(cat "$K/COMMIT")"
if [ -s "$K/DIRTY" ]; then
	echo "WARNING: kernel built from a dirty tree" >&2
fi
[ -f "$PATCHED" ] || { echo "missing $PATCHED: run scripts/pcie/build-m1n1.sh" >&2; exit 1; }
echo "m1n1: $(head -1 "$ROOT/build/m1n1-pcie/COMMIT")"

INITRD="${1:-$K/initramfs-wifi.gz}"
if [ ! -f "$INITRD" ] && [ "$INITRD" = "$K/initramfs-wifi.gz" ]; then
	"$ROOT/scripts/pcie/mkinitramfs-wifi.sh" >/dev/null
fi
echo "initramfs: $INITRD"

rm -f "$K"/m1n1-linux-pcie*.bin

EXTRA=(panic=0 nvme_core.admin_timeout=15 ${2+"${@:2}"})

# payload NAME [bootargs...]
payload() {
	local name="$1"
	shift
	M1N1="$PATCHED" TREE=pcie "$ROOT/scripts/mkpayload.sh" "$INITRD" \
		"${EXTRA[@]}" "$@" | sed 's/^.*bootargs:/  bootargs:/'
	cp "$ROOT/build/m1n1-linux.bin" "$K/m1n1-linux-pcie-$name.bin"
	echo "$K/m1n1-linux-pcie-$name.bin"
}

# brcmfmac.debug: TRACE | INFO | PCIE | FWCON (needs CONFIG_BRCMDBG)
payload r7-debug nvme.apple_ans_debug=1 brcmfmac.debug=0x180006
payload r7-aspm pcie_apple_h9p.aspm_l1_ports=0x2
payload r7
