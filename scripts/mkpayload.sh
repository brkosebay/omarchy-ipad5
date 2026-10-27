#!/usr/bin/env bash
# Assemble the m1n1 chainload payload: m1n1.bin + bootargs + dtbs + kernel + initramfs.
# m1n1 picks the dtb whose compatible matches the ADT, so all four iPad 5 dtbs
# (S8000/S8003 x Wi-Fi/cellular) go in and the right one is chosen on device.
#
# Usage: [TREE=name] scripts/mkpayload.sh [initramfs.gz] [extra bootargs...]
#   TREE picks a workstream kernel (build/kernel-<name>, see build-kernel.sh --tree).
#   PAYLOAD_OUT overrides the output file; M1N1_VARS adds m1n1 payload variables
#   (newline-separated key=value lines, e.g. M1N1_VARS=psci=1, see
#   scripts/boot/mkpayload.sh).
# Output: build/m1n1-linux.bin
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K="$ROOT/build/kernel${TREE:+-$TREE}"
M1N1="${M1N1:-$ROOT/build/prebuilt/m1n1/m1n1.bin}"   # override e.g. M1N1=build/m1n1-pcie/m1n1.bin
INITRD="${1:-$ROOT/build/prebuilt/initramfs/initramfs.gz}"
shift || true
# Kernel log goes to the framebuffer; HoolockRD spawns its own shell on the
# USB ACM gadget (configfs) and telnet on 172.16.42.1.
BOOTARGS="earlycon console=tty0 loglevel=7 ${*:-}"
OUT="${PAYLOAD_OUT:-$ROOT/build/m1n1-linux.bin}"

# J71T_DTB overrides the j71t board DTB; use a matching, validated tree.
DTBS=("$K"/dtbs/s8000-j71s.dtb "$K"/dtbs/s8000-j72s.dtb \
      "$K/dtbs/${J71T_DTB:-s8003-j71t.dtb}" "$K"/dtbs/s8003-j72t.dtb)

for f in "$M1N1" "$K/Image.gz" "$INITRD" "${DTBS[@]}"; do
	[ -f "$f" ] || { echo "missing: $f (run fetch-prebuilts.sh / build-kernel.sh)" >&2; exit 1; }
done

# m1n1 unpacks every gzip member as its own payload, and each cpio it finds
# *replaces* the initrd (kboot_set_initrd). So an initramfs made of several
# concatenated .gz archives (base + firmware overlay) would boot with only the
# last one: no /init, then "VFS: Unable to mount root fs". Repack into a
# single gzip stream; the kernel itself unpacks concatenated cpio archives.
SINGLE="$ROOT/build/.initrd-single${PAYLOAD_OUT:+-$(basename "$PAYLOAD_OUT" .bin)}.gz"
gzip -dc "$INITRD" | gzip -9n > "$SINGLE"
INITRD="$SINGLE"

VARS=""
[ -n "${M1N1_VARS:-}" ] && VARS="$(printf '%s\n' "$M1N1_VARS")"$'\n'
cat "$M1N1" <(printf '%s' "$VARS"; echo "chosen.bootargs=$BOOTARGS") "${DTBS[@]}" \
	"$K/Image.gz" "$INITRD" > "$OUT"
echo "$OUT ($(du -h "$OUT" | cut -f1)) ${M1N1_VARS:+vars: $(echo $M1N1_VARS) }bootargs: $BOOTARGS"
