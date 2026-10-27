#!/usr/bin/env bash
# S0/S1 probe: boot sepOS on an A9 (s8000/s8003) from pongoOS.
#
# The current iBoot-assisted pongoOS chain supplies /chosen/memory-map:SEPFW
# but not "sepfw-booted". The patched `sep xnu` command drives the verified
# AppleSEPBooter cold path against that staged image. The upload/adopt path is
# retained only as a legacy diagnostic; the extracted disk file is not proven
# equivalent to iBoot's staged object.
#
# Usage:
#   scripts/sep/probe-sep.sh pongo          # put the iPad in DFU when told
#   scripts/sep/probe-sep.sh sep [fw.img4]  # stage the fw, boot it, dump messages
#   scripts/sep/probe-sep.sh shell          # interactive pongo shell
#
# Notes:
#   - "sep pwn" is A10-only in pongoOS (the fwload race uses 0x210E00000, the
#     A10 AOP SRAM). A9 does not need it to boot the stock sepOS.
#   - A malformed/invalid IMG4 makes the SEP panic for the rest of this boot.
#     That is harmless (reset brings it back) but it costs a DFU cycle.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PONGO_TERM="$ROOT/build/bin/pongoterm"
PONGO_BIN="$ROOT/build/prebuilt/pongo/Pongo-a9sep.bin"
# Candidate A: the image as stored in the APFS "iBoot" volume (payload plus the
# 6 KiB trailing blob, which may be the encrypted SEP manifest).
# Candidate B: payload + the plaintext IM4M taken from the iBoot bundle, which
# carries the sepi digest a550f6ab... (build/sep/full/).
DEFAULT_FW="$ROOT/build/sep/sep.j71t.img4"
LOG_DIR="$ROOT/docs/sep/logs"

run_pongo_term() {
	# $1: seconds to keep the link open, rest: lines to send
	local secs="$1"; shift
	local out; out="$(mktemp -t sep-probe)"
	( printf '%s\n' "$@"; sleep "$secs" ) | "$PONGO_TERM" >"$out" 2>&1 &
	local pid=$!
	local waited=0
	while kill -0 "$pid" 2>/dev/null && [ $waited -lt "$secs" ]; do
		sleep 1; waited=$((waited + 1))
	done
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	cat "$out"
	mkdir -p "$LOG_DIR"
	cp "$out" "$LOG_DIR/$(date +%Y%m%d-%H%M%S)-sep.log"
	rm -f "$out"
}

case "${1:-}" in
pongo)
	[ -f "$PONGO_BIN" ] || { echo "missing $PONGO_BIN (build pongoOS with patches/pongoos-a9-sep.patch)" >&2; exit 1; }
	echo "[*] pongo sha256=$(shasum -a 256 "$PONGO_BIN" | awk '{print $1}')"
	echo "Put the iPad in DFU now (palera1n -D helps)."
	# -l: rootless, -p: stop in pongoOS, -k: patched Pongo.bin
	PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp -k "$PONGO_BIN"
	;;
sep)
	FW="${2:-$DEFAULT_FW}"
	[ -f "$FW" ] || { echo "no SEP firmware at $FW" >&2; exit 1; }
	echo "WARNING: legacy upload/adopt test; use pongo-sep.sh --xnu status for the staged image" >&2
	echo "[*] staging $FW"
	# keep the link open long enough for fwload + SEPOS boot.
	# 'tz' first: on a pongoOS boot the TZ0 range comes from the bootrom, we do
	# not program it ourselves. If tz reports base 0x800000000 / size 0x1000 the
	# bootrom did not set it and 'sep sboot' will not work yet.
	run_pongo_term 45 "/send $FW" "sep adopt" "tz" "sep stat" "sep sboot" "sep stat" "sep msg fd 0 0 0 0"
	;;
shell)
	exec "$PONGO_TERM"
	;;
*)
	sed -n '2,22p' "$0"
	exit 1
	;;
esac
