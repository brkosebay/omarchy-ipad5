#!/usr/bin/env bash
# Cold-boot helper for the Mac (docs/boot/warm-reboot.md). When the iPad
# appears in DFU mode (the owner held the buttons), run the whole tethered
# chain with the default payload: palera1n (checkm8) -> pongoOS -> /send
# payload -> bootm, plus the second bootm pongoOS sometimes needs. Recovery
# mode only gets a notification: DFU needs the buttons.
#
#   scripts/boot/autoboot.sh           watch forever (what the LaunchAgent runs)
#   scripts/boot/autoboot.sh --once    act on the current USB state once, then exit
#   scripts/boot/autoboot.sh --state   print dfu|recovery|pongo|m1n1|linux|ios|none
#   scripts/boot/install-autoboot.sh   install/remove the LaunchAgent
#
# Pause it with `touch build/boot/autoboot.disabled` while booting by hand.
# Payload: $LINUX_A9_PAYLOAD, else build/boot/default-payload (a symlink the
# orchestrator points at the current payload), else
# build/nvme-install/m1n1-linux-nvme.bin. Log: build/boot/autoboot.log.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/build/boot"
LOG="$B/autoboot.log"
PONGOTERM="$ROOT/build/bin/pongoterm"
PONGO_BIN="$ROOT/build/prebuilt/pongo/Pongo.bin"

# Opt-in SEP boot (docs/sep/README.md): with SEP_BOOT=1 the patched pongoOS is
# used, sepOS is staged and booted before the payload, and the SEP stays alive
# across bootm. Off by default; a failure only logs and the Linux boot goes on.
SEP_BOOT="${SEP_BOOT:-0}"
SEP_IMG="${SEP_IMG:-$ROOT/build/sep/sep.j71t.img4}"
if [ "$SEP_BOOT" = 1 ] && [ -f "$ROOT/build/prebuilt/pongo/Pongo-a9sep.bin" ]; then
	PONGO_BIN="$ROOT/build/prebuilt/pongo/Pongo-a9sep.bin"
fi
mkdir -p "$B"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

log() { echo "$(date '+%F %T') $*" | tee -a "$LOG" >&2; }
notify() {
	log "$*"
	osascript -e "display notification \"$*\" with title \"linux-a9 autoboot\"" >/dev/null 2>&1 || true
}

payload() {
	local p="${LINUX_A9_PAYLOAD:-}"
	[ -n "$p" ] || { [ -e "$B/default-payload" ] && p="$B/default-payload"; }
	[ -n "$p" ] || p="$ROOT/build/nvme-install/m1n1-linux-nvme.bin"
	echo "$p"
}

# USB IDs: Apple 0x05ac with DFU 0x1227, recovery 0x1281, pongoOS 0x4141,
# iOS 0x12ab, the Linux gadget 0x4142; m1n1's proxy is 0x1209:0x316d
state() {
	local ids
	ids=$(ioreg -p IOUSB -l -w0 | awk '
		/\+-o /{ if (v != "" && p != "") print v ":" p; v = ""; p = "" }
		/"idVendor" = /{ sub(/.*= /, ""); v = $0 }
		/"idProduct" = /{ sub(/.*= /, ""); p = $0 }
		END { if (v != "" && p != "") print v ":" p }')
	case "$ids" in
	*1452:4647*) echo dfu ;;
	*1452:4737*) echo recovery ;;
	*1452:16705*) echo pongo ;;
	*1452:4779*) echo ios ;;
	*1452:16706*) echo linux ;;
	*4617:12653*) echo m1n1 ;;
	*) echo none ;;
	esac
}

wait_state() { # wait_state WANT SECONDS (WANT may be !STATE)
	local want=$1 t=$2 s
	while [ "$t" -gt 0 ]; do
		s=$(state)
		case "$want" in
		!*) [ "$s" != "${want#!}" ] && return 0 ;;
		*) [ "$s" = "$want" ] && return 0 ;;
		esac
		sleep 1
		t=$((t - 1))
	done
	return 1
}

send_payload() {
	local p
	p=$(payload)
	[ -f "$p" ] || { notify "no payload at $p"; return 1; }
	log "pongoOS: sending $(readlink "$p" 2>/dev/null || echo "$p")"
	printf '/send %s\nbootm\n' "$p" | "$PONGOTERM" >>"$LOG" 2>&1
	wait_state '!pongo' 15 && return 0
	log "pongoOS kept the payload (first bootm eaten): second bootm"
	printf 'bootm\n' | "$PONGOTERM" >>"$LOG" 2>&1
	wait_state '!pongo' 15 && return 0
	log "still in pongoOS: sending the payload again"
	printf '/send %s\nbootm\n' "$p" | "$PONGOTERM" >>"$LOG" 2>&1
	wait_state '!pongo' 20
}

boot_from_dfu() {
	notify "iPad in DFU: checkm8 -> pongoOS -> Linux"
	(cd "$ROOT" && PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp -k "$PONGO_BIN") >>"$LOG" 2>&1 &
	local pid=$!
	# palera1n can hang after "Booting pongoOS..." while pongoOS is already up
	if wait_state pongo 120; then
		kill "$pid" 2>/dev/null
		wait "$pid" 2>/dev/null
		boot_from_pongo
	else
		kill "$pid" 2>/dev/null
		wait "$pid" 2>/dev/null
		notify "checkm8/pongoOS failed (state $(state)); re-enter DFU to retry"
		return 1
	fi
}

boot_from_pongo() {
	sleep 2 # let pongoOS finish enumerating
	if [ "$SEP_BOOT" = 1 ]; then
		if [ "${SEP_MODE:-}" = xnu ]; then
			log "SEP_BOOT=1: AppleSEPBooter cold sequence (ART=${SEP_ART:-none})"
			sep_args=(--xnu status)
			if [ -n "${SEP_ART:-}" ]; then
				sep_args+=(art "$SEP_ART")
			fi
			"$ROOT/scripts/sep/pongo-sep.sh" "${sep_args[@]}" >>"$LOG" 2>&1
			rc=$?
		else
			log "SEP_BOOT=1: staging sepOS from $SEP_IMG"
			"$ROOT/scripts/sep/pongo-sep.sh" "$SEP_IMG" >>"$LOG" 2>&1
			rc=$?
			# Optional second candidate, same pongoOS session, only if the first
			# failed without a SEPROM panic (a panic leaves the SEP unusable).
			if { [ "$rc" = 1 ] || [ "$rc" = 3 ]; } && [ -n "${SEP_IMG2:-}" ]; then
				log "SEP: first image did not boot; trying $SEP_IMG2"
				"$ROOT/scripts/sep/pongo-sep.sh" "$SEP_IMG2" >>"$LOG" 2>&1
				rc=$?
			fi
		fi
		case $rc in
		0) log "sepOS booted; the SEP stays alive across bootm" ;;
		4) log "SEP cold command completed (0x6a); liveness unconfirmed; continuing to Linux" ;;
		2) log "SEPROM panic: sepOS image rejected (SEP unusable until reset); continuing" ;;
		*) log "SEP step failed; continuing to Linux" ;;
		esac
	fi
	if ! send_payload; then
		notify "pongoOS did not boot the payload; check $LOG"
		return 1
	fi
	log "m1n1 running, waiting for Linux"
	if wait_state linux 120; then notify "Linux is up (USB gadget present)"; else notify "no Linux USB gadget after 120 s: check the screen and $LOG"; fi
}

act() {
	[ -e "$B/autoboot.disabled" ] && return 0
	# A boot driven by hand (scripts/boot.sh) owns the USB device
	if pgrep -x palera1n >/dev/null || pgrep -f "$PONGOTERM" >/dev/null; then
		log "palera1n/pongoterm already running: manual boot in progress, not interfering"
		return 0
	fi
	case "$(state)" in
	dfu) boot_from_dfu ;;
	pongo) boot_from_pongo ;;
	recovery) notify "iPad in recovery mode: hold the buttons for DFU to boot Linux" ;;
	esac
}

case "${1:-}" in
--state) state ;;
--once) act ;;
"")
	pidf="$B/autoboot.pid"
	if [ -f "$pidf" ] && kill -0 "$(cat "$pidf")" 2>/dev/null; then
		log "already running (pid $(cat "$pidf"))"
		exit 0
	fi
	echo $$ >"$pidf"
	log "watching USB (payload $(payload))"
	last=none
	while :; do
		s=$(state)
		if [ "$s" != "$last" ]; then
			log "state $last -> $s"
			case "$s" in dfu | recovery) act ;; esac
			s=$(state)
		fi
		last=$s
		sleep 2
	done
	;;
*) sed -n '2,17p' "$0"; exit 2 ;;
esac
