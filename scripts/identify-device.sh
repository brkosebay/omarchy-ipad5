#!/usr/bin/env bash
# Identify the connected iPad 5: which A9 (Samsung S8000 / TSMC S8003), which
# board (j71s/j71t Wi-Fi, j72s/j72t cellular), iOS version. Works with the
# iPad booted normally (unlocked + trusted) or in recovery/DFU.
set -euo pipefail

if ideviceinfo -k ProductType >/dev/null 2>&1; then
	product="$(ideviceinfo -k ProductType)"
	board="$(ideviceinfo -k HardwareModel)"
	cpid="$(printf '0x%04x' "$(ideviceinfo -k ChipID)")"
	echo "mode:     normal (iOS $(ideviceinfo -k ProductVersion) $(ideviceinfo -k BuildVersion))"
elif irecovery -q >/dev/null 2>&1; then
	q="$(irecovery -q)"
	product="$(sed -n 's/^PRODUCT: //p' <<<"$q")"
	board="$(sed -n 's/^MODEL: //p' <<<"$q")"
	cpid="$(sed -n 's/^CPID: //p' <<<"$q")"
	echo "mode:     $(sed -n 's/^MODE: //p' <<<"$q")"
else
	echo "No device found. Plug the iPad in, unlock it and tap Trust (or put it in DFU)." >&2
	exit 1
fi

board="$(tr '[:upper:]' '[:lower:]' <<<"$board" | sed 's/ap$//')"
case "$cpid" in
	0x8000) soc="s8000 (A9, Samsung)" ;;
	0x8003) soc="s8003 (A9, TSMC)" ;;
	*) soc="unknown ($cpid) - not an A9?" ;;
esac

echo "product:  $product"
echo "board:    $board"
echo "soc:      $soc"
echo "dtb:      s${cpid#0x}-$board.dtb"
echo "adt:      docs/adt/$board.txt"
