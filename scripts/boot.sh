#!/usr/bin/env bash
# Tethered boot: checkm8 (palera1n) -> pongoOS -> m1n1 -> Linux.
# Nothing is written to the iPad's disk; a normal reboot goes back to iOS.
#
# Usage:
#   scripts/boot.sh pongo        # put the iPad in DFU when told; boots pongoOS
#   scripts/boot.sh linux [file] # send build/m1n1-linux.bin (or the given payload) and boot it
#   scripts/boot.sh m1n1         # boot bare m1n1 (proxy mode, for poking hardware)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PONGOTERM="$ROOT/build/bin/pongoterm"

# Send a file to pongoOS and bootm it. Right after palera1n boots pongoOS, the
# first pongoterm often fails with "USBDeviceOpenSeize: exclusive access" and
# then hangs, so give each attempt 20 s to report "Uploaded" and retry.
send_bootm() {
	local out tries=0
	out="$(mktemp -t pongoterm)"
	while [ $tries -lt 4 ]; do
		tries=$((tries + 1))
		: >"$out"
		printf '/send %s\nbootm\n' "$1" | "$PONGOTERM" >"$out" 2>&1 &
		local pid=$! waited=0
		while kill -0 "$pid" 2>/dev/null && ! grep -q 'Uploaded' "$out" && [ $waited -lt 20 ]; do
			sleep 1; waited=$((waited + 1))
		done
		if grep -q 'Uploaded' "$out"; then
			wait "$pid" 2>/dev/null || true
			cat "$out"; rm -f "$out"; return 0
		fi
		kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
		echo "pongoterm attempt $tries: no upload ($(grep -o 'USB[A-Za-z]*: [^[]*' "$out" | head -1)); retrying" >&2
		sleep 3
	done
	cat "$out"; rm -f "$out"; return 1
}

case "${1:-}" in
pongo)
	# -l: rootless, -p: stop in pongoOS, -k: our Pongo.bin.
	# It may hang after "Booting pongoOS..." while the iPad shows Pongo; Ctrl-C is fine then.
	PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp -k "$ROOT/build/prebuilt/pongo/Pongo.bin"
	;;
linux)
	PAYLOAD="${2:-$ROOT/build/m1n1-linux.bin}"
	[ -f "$PAYLOAD" ] || { echo "no such payload: $PAYLOAD" >&2; exit 1; }
	send_bootm "$PAYLOAD"
	;;
m1n1)
	send_bootm "$ROOT/build/prebuilt/m1n1/m1n1.bin"
	;;
*)
	sed -n '2,9p' "$0"
	exit 1
	;;
esac
