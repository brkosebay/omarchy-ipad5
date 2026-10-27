#!/usr/bin/env bash
# Opt-in: boot sepOS from pongoOS before the Linux payload is sent.
#
# Sends the personalized sep-firmware IMG4, adopts it as pongoOS's staged SEP
# firmware and runs the stock cold-boot sequence (tz0 lockdown -> boot_tz0 ->
# optional ART -> IMG4 load/boot).
# The SEP then stays alive across bootm, so Linux starts with sepOS running.
#
# Requires the patched pongoOS (sep adopt / sep sboot / sep stat):
#   build/prebuilt/pongo/Pongo-a9sep.bin
# Stock pongoOS has no such commands.
#
# Never fatal to the caller: exits non-zero on failure, but the Linux payload
# can still be booted afterwards (the caller decides). Bounded in time.
#
# Usage: scripts/sep/pongo-sep.sh [IMG4] [timeout-seconds]
#        scripts/sep/pongo-sep.sh --inspect
#        scripts/sep/pongo-sep.sh --staged [timeout-seconds]
#        scripts/sep/pongo-sep.sh --xnu [status] [noshmem] [art [ART.bin]] [split32]
#
# --xnu runs the AppleSEPBooter order against iBoot's staged SEPFW.
# A9 default (no ART): boot_tz0 -> fwload/boot. Opcode 0x08 is resume-only
# and is deliberately not sent on this cold path.
# Add "art" to include the ART step, "status" for the op 0x02 checks,
# The root-buffer descriptor is included by default; "noshmem" disables it
# as a negative control. "ping" enables the A10-style ping. "mode1" is an
# explicit firmware-mode discriminator. TMM is intentionally unavailable
# until real manifest and patch physical addresses are known.
# A9 boot-handshake sends use one 64-bit MMIO store by default, matching XNU.
# `split32` selects the previous paired-32-bit path as a controlled baseline.
# --xnu sends at most one SEP sequence. It may retry opening pongoterm only
# when no device was reached. `art` without ART.bin stages a zero page as an
# explicit diagnostic; ART is otherwise skipped on this A9 board.
# The discovery build keeps draining after "SEPOS BOOTED" until the mailbox
# has been continuously empty for 3 s (10 s hard bound), then prints SEP-DISC
# rows and the retained raw stream. For --xnu, exit 0 requires that final table.
# Exit: 0 = pongoOS printed "SEPOS BOOTED" and (for --xnu) the discovery table
#       4 = op 6 completed (0x6a), but no unsolicited sepOS control message
#       2 = SEPROM panic (SEP unusable until reset)
#       1 = anything else (timeout, upload failure, stock pongoOS)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODE=upload
ART=""
XNU_ARGS=""
XNU_TIMEOUT=""
case "${1:-}" in
--inspect)
	MODE=inspect
	shift
	;;
--staged)
	MODE=staged
	shift
	;;
--xnu)
	# AppleSEPBooter sequence against iBoot's staged SEPFW.
	# A9 default: boot_tz0 -> fwload/boot (no ART, no resume opcode).
	MODE=xnu
	shift
	XNU_ARGS=""
	while [ $# -gt 0 ]; do
		case "$1" in
		noart|noshmem|mode1|status|ping|art|shmem|split32) XNU_ARGS="$XNU_ARGS $1"; shift ;;
		*) break ;;
		esac
	done
	case " $XNU_ARGS " in
	*" art "*)
		# A non-numeric positional argument after `art` is the token path.
		# A numeric argument is the host watchdog timeout for zero-page ART.
		if [ $# -gt 0 ]; then
			case "$1" in
			*[!0-9]*) ART="$1"; shift ;;
			esac
		fi
		;;
	esac
	if [ $# -gt 0 ]; then
		XNU_TIMEOUT="$1"
		shift
	fi
	if [ $# -gt 0 ]; then
		echo "pongo-sep: unexpected --xnu argument: $1" >&2
		exit 1
	fi
	;;
esac
if [ "$MODE" = inspect ]; then
	IMG=""
	TIMEOUT="${1:-10}"
elif [ "$MODE" = xnu ]; then
	IMG="$ROOT/build/sep/sep.j71t.img4"
	# With both status checks enabled, the fully bounded path can spend 50 s +
	# 60 s + 3 s + 50 s + 60 s + 60 s + 10 s before the discovery dump. The
	# runner exits early when the dump appears; this is only the hard watchdog.
	TIMEOUT="${XNU_TIMEOUT:-330}"
else
	IMG="${1:-$ROOT/build/sep/sep.j71t.img4}"
	TIMEOUT="${2:-25}"
fi
case "$TIMEOUT" in
''|*[!0-9]*)
	echo "pongo-sep: timeout must be an integer number of seconds: $TIMEOUT" >&2
	exit 1
	;;
esac
PONGOTERM="$ROOT/build/bin/pongoterm"
LOG="$ROOT/build/sep/pongo-sep.log"

mkdir -p "$(dirname "$LOG")"

if [ "$MODE" = upload ] && [ ! -f "$IMG" ]; then
	echo "pongo-sep: no image at $IMG" >&2
	exit 1
fi
if [ "$MODE" = xnu ] && [ -n "$ART" ] && [ ! -f "$ART" ]; then
	echo "pongo-sep: no ART file at $ART" >&2
	exit 1
fi
if [ ! -x "$PONGOTERM" ]; then
	echo "pongo-sep: no pongoterm at $PONGOTERM" >&2
	exit 1
fi

run_once() {
	local tmp out fifo terminal=0
	tmp=$(mktemp -d)
	out="$tmp/out"
	fifo="$tmp/in"
	mkfifo "$fifo"
	# Keep the FIFO writer open while pongoOS runs. This holds pongoterm's USB
	# session without a watchdog-sized `sleep` process delaying early success.
	"$PONGOTERM" <"$fifo" >"$out" 2>&1 &
	local pid=$! waited=0
	exec 9>"$fifo"
	{
		if [ "$MODE" = inspect ]; then
			printf 'sep info\nsep stat\n'
		elif [ "$MODE" = xnu ]; then
			stage_art=0
			case " $XNU_ARGS " in
			*" art "*) stage_art=1 ;;
			esac
			if [ "$stage_art" = 1 ]; then
				if [ -n "$ART" ]; then
					printf '/send %s\nsep art\n' "$ART"
				else
					printf 'sep art\n'
				fi
			fi
			printf 'sep info\nsep xnu %s\nsep stat\n' "$XNU_ARGS"
		elif [ "$MODE" = staged ]; then
			# iBoot already staged a complete, personalized IMG4 in
			# /chosen/memory-map:SEPFW; use it instead of uploading ours.
			printf 'sep info\nsep sboot\nsep stat\n'
		else
			printf '/send %s\nsep adopt\nsep info\nsep sboot\nsep stat\n' "$IMG"
		fi
	} >&9
	while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt $((TIMEOUT + 5)) ]; do
		sleep 1
		waited=$((waited + 1))
		if [ "$MODE" = xnu ]; then
			# Do not stop at SEPOS BOOTED: the discovery table follows after
			# Pongo's bounded passive-drain window.
			if grep -q "sep discovery: end" "$out"; then
				terminal=1
				break
			fi
			if grep -qE "SEPROM panic|no [[:xdigit:]]{2} ack|no unsolicited sepOS control message after 60s|already attempted in this boot|; stopping" "$out"; then
				break
			fi
		fi
	done
	# Give pongoterm one final turn to flush the line containing the marker.
	[ "$terminal" = 1 ] && sleep 1
	exec 9>&-
	kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	cat "$out"
	rm -f "$fifo" "$out"
	rmdir "$tmp"
}

# The first pongoterm after palera1n often fails to seize the USB device; retry
# a couple of times, same as scripts/boot.sh does for the payload.
for try in 1 2 3; do
	OUT="$(run_once)"
	printf '%s\n' "$OUT" >>"$LOG"
	if [ "$MODE" = inspect ]; then
		printf '%s\n' "$OUT"
		if printf '%s' "$OUT" | grep -q "staged SEPFW"; then
			echo "pongo-sep: inspection only; SEP boot state was not changed" >&2
			exit 0
		fi
		echo "pongo-sep: inspection failed; no staged SEPFW report" >&2
		exit 1
	fi

	if [ "$MODE" = xnu ] && printf '%s' "$OUT" | grep -q "SEPOS BOOTED"; then
		if ! printf '%s' "$OUT" | grep -q "sep discovery: end"; then
			echo "pongo-sep: sepOS booted but discovery capture did not finish; see $LOG" >&2
			exit 1
		fi
		# Make the table visible in autoboot.log as well as the full capture log.
		printf '%s\n' "$OUT" | grep -E "SEP-DISC|sep discovery: end|sep capture: total|post-boot capture" || true
		echo "pongo-sep: sepOS booted; discovery capture complete" >&2
		exit 0
	fi
	if printf '%s' "$OUT" | grep -q "SEPOS BOOTED"; then
		echo "pongo-sep: sepOS booted" >&2
		exit 0
	fi
	if printf '%s' "$OUT" | grep -q "SEPROM panic"; then
		echo "pongo-sep: SEPROM panic (sepOS image rejected); SEP unusable until reset" >&2
		exit 2
	fi
	if [ "$MODE" = xnu ] && printf '%s' "$OUT" | grep -q "fwload/boot: acked 6a"; then
		echo "pongo-sep: cold op 6 completed; sepOS liveness is unconfirmed (no unsolicited ep0/op0d)" >&2
		exit 4
	fi
	# Deterministic refusal from an older Pongo-a9sep.bin: retrying cannot help.
	if printf '%s' "$OUT" | grep -q "tz0 already locked, cannot boot sepOS"; then
		echo "pongo-sep: pongoOS refused because TZ0 is locked (old binary); exit 3" >&2
		exit 3
	fi
	# If pongoterm could not reach the device at all, retrying is pointless.
	if printf '%s' "$OUT" | grep -qiE "no device|unable to open|exclusive access"; then
		echo "pongo-sep: pongoterm could not talk to pongoOS (try $try); retrying" >&2
		sleep 3
		continue
	fi
	echo "pongo-sep: no SEPOS boot message (try $try); see $LOG" >&2
	# A second SEP sequence in the same boot is invalid even if the first
	# sequence timed out. Only the no-device branch above is safe to retry.
	if [ "$MODE" = xnu ]; then
		break
	fi
	sleep 2
done

echo "pongo-sep: gave up; Linux boot is unaffected" >&2
exit 1
