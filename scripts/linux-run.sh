#!/usr/bin/env bash
# Run shell commands on the iPad's Linux (HoolockRD) over the USB ACM serial
# console and print their output. Output is framed by markers, so prompts and
# echo noise are stripped and the remote exit status becomes ours.
#
# Usage: scripts/linux-run.sh 'dmesg | tail'            # one command line
#        scripts/linux-run.sh -f local-script.sh         # upload + run a script
#        TIMEOUT=60 scripts/linux-run.sh 'long command'  # default 30s
# Raw transcripts are appended to build/serial-transcript.log.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TTY="${LINUX_TTY:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
TIMEOUT="${TIMEOUT:-30}"
[ -n "$TTY" ] || { echo "no /dev/cu.usbmodem*: is Linux booted and USB gadget up?" >&2; exit 2; }

if [ "${1:-}" = "-f" ]; then
	# Upload the script via a heredoc into /tmp, then run it.
	name="/tmp/$(basename "$2")"
	CMD="cat > $name <<'__LINUX_A9_EOF__'
$(cat "$2")
__LINUX_A9_EOF__
sh $name"
else
	CMD="$*"
fi

TAG="LA9$$$RANDOM"
LOG="$ROOT/build/serial-transcript.log"
OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT

exec 3<>"$TTY"
stty -f "$TTY" 115200 raw -echo 2>/dev/null || true
cat <&3 >"$OUT" & CATPID=$!
# The ACM port appears before HoolockRD's shell listens on it: poke with a
# newline until a root prompt ('# ') shows up, then send the command.
for _ in $(seq 1 40); do
	printf '\r' >&3
	sleep 0.5
	grep -qa '# ' "$OUT" && break
done
# Markers are split with '' so the echoed command line can't match them.
printf '%s\r' "echo ${TAG}''_BEGIN; ( $CMD ) 2>&1; echo ${TAG}''_END:\$?" >&3

deadline=$(( $(date +%s) + TIMEOUT ))
while ! grep -qa "${TAG}_END:" "$OUT"; do
	if [ "$(date +%s)" -ge "$deadline" ]; then
		kill "$CATPID" 2>/dev/null; exec 3>&-
		{ echo "=== $(date) TIMEOUT: $CMD"; cat "$OUT"; } >>"$LOG"
		echo "TIMEOUT after ${TIMEOUT}s; partial output:" >&2
		sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' "$OUT" | tr -d '\r' | sed -n "/${TAG}_BEGIN/,\$p" | tail -n +2 >&2
		exit 124
	fi
	sleep 0.3
done
kill "$CATPID" 2>/dev/null; exec 3>&-
{ echo "=== $(date) $CMD"; cat "$OUT"; } >>"$LOG"

CLEAN="$(sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' "$OUT" | tr -d '\r')"
printf '%s\n' "$CLEAN" | sed -n "/^${TAG}_BEGIN\$/,/^${TAG}_END:/p" | sed '1d;$d'
rc="$(printf '%s\n' "$CLEAN" | sed -n "s/^${TAG}_END:\([0-9]*\).*/\1/p" | tail -1)"
exit "${rc:-1}"
