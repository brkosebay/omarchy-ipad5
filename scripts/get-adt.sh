#!/usr/bin/env bash
# Pull Apple DeviceTrees (ADT) straight out of Apple's IPSWs (remote partial
# download, only the DeviceTree files) and dump them as text + JSON.
#
# The ADT is the map of every hardware block: MMIO ranges, IRQs, compatibles,
# GPIOs, and per-board tuning. It is the primary input for writing Linux DTS
# nodes and drivers.
#
# Usage: scripts/get-adt.sh [ProductType ...]
#   default: iPad6,11 iPad6,12 (iPad 5 Wi-Fi/cellular -> j71s/j71t/j72s/j72t)
#   e.g. scripts/get-adt.sh iPad7,12   (iPad 7 / A10, for diffing against Pauli1Go's work)
# Output: docs/adt/<board>.txt, docs/adt/<board>.json
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/docs/adt"
DL="$ROOT/build/adt-dl"
mkdir -p "$OUT" "$DL"

DEVICES=("$@")
[ ${#DEVICES[@]} -gt 0 ] || DEVICES=(iPad6,11 iPad6,12)

for dev in "${DEVICES[@]}"; do
	echo "[*] $dev: latest IPSW DeviceTrees"
	ipsw download ipsw -d "$dev" --latest -y -f -o "$DL/$dev" \
		--pattern 'DeviceTree\.[a-z0-9]+ap\.im4p$'
done

find "$DL" -name 'DeviceTree.*.im4p' | while read -r im4p; do
	board="$(basename "$im4p" .im4p | sed 's/^DeviceTree\.//; s/ap$//')"
	raw="$DL/$board.dtre"
	ipsw img4 im4p extract -o "$raw" "$im4p" >/dev/null
	ipsw dtree --no-color "$raw" > "$OUT/$board.txt"
	ipsw dtree --json "$raw" > "$OUT/$board.json"
	echo "    $board -> docs/adt/$board.{txt,json}"
done
