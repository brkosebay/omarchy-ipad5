#!/usr/bin/env bash
# Capture the SEP command trace from the iPad (iOS side).
#
# AppleSEPGenericTransfer logs every command it sends:
#   AppleSEPGenericTransfer::%s: command:0x%x, inData:%p, inLength:%zu, ...
# and AppleMesaSEPDriver / IOSEPBiometricService log their side too. The first
# enrolment is the interesting one: it includes the catacomb / user-bag
# creation that later enrolments skip.
#
# The capture runs until stopped. There is no timeout: stop it with
#   scripts/sep/trace-sbio.sh --stop
# or Ctrl-C in a terminal.
#
# Two transports, because the permissions differ:
#
#   syslog (default)  `idevicesyslog` relays the device syslog over USB
#                     through lockdownd. No ssh, no root, no entitlements.
#   --ssh             `log stream --level debug` on the device. Needs a
#                     reachable sshd and, for kernel/debug streams, root.
#
# Usage:
#   scripts/sep/trace-sbio.sh [out.log]          # run until --stop
#   scripts/sep/trace-sbio.sh --ssh [out.log]
#   scripts/sep/trace-sbio.sh --stop [out.log]   # stop and report
#   scripts/sep/trace-sbio.sh --check            # permissions preflight
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODE=syslog
ACTION=start
OUT=""

while [ $# -gt 0 ]; do
	case "$1" in
	--ssh) MODE=ssh; shift ;;
	--stop) ACTION=stop; shift ;;
	--check) ACTION=check; shift ;;
	*) OUT="$1"; shift ;;
	esac
done

UDID="${IPAD_UDID:-$(idevice_id -l 2>/dev/null | head -1)}"
OUT="${OUT:-$ROOT/build/sep/sbio-trace.log}"
PIDFILE="${OUT}.pid"

# Wide enough to catch the enrol sequence end to end: SBIO commands, the
# catacomb/identity bookkeeping and the keystore side that creates the user bag
# on a first enrolment. Payload bytes are <private> in os_log; opcodes and
# lengths are visible, which is what the driver needs.
FILTER='AppleSEP|AppleMesa|IOSEPBiometric|AppleBiometric|BiometricKit|catacomb|Catacomb|sep-bio'

preflight() {
	echo "== device"
	[ -n "$UDID" ] && echo "udid: $UDID" || echo "no paired device over USB"
	echo "== tools"
	command -v idevicesyslog >/dev/null && echo "idevicesyslog: yes" || echo "idevicesyslog: MISSING"
	if "$ROOT/scripts/ipad-ssh.sh" 'id -u' 2>/dev/null; then
		echo "ssh: yes (uid above; 501 = mobile, 0 = root)"
		"$ROOT/scripts/ipad-ssh.sh" 'sudo -n true' 2>/dev/null \
			&& echo "sudo: passwordless" || echo "sudo: needs the owner's password"
	else
		echo "ssh: not reachable"
	fi
	echo "== capture"
	if [ -f "$PIDFILE" ]; then
		echo "running (pid $(cat "$PIDFILE")) -> $OUT"
	else
		echo "not running"
	fi
}

stop_capture() {
	local killed=0
	if [ -f "$PIDFILE" ]; then
		local pid; pid="$(cat "$PIDFILE")"
		kill "$pid" 2>/dev/null && killed=1 || true
		rm -f "$PIDFILE"
	fi
	# The relay is the source of the pipeline; killing it ends grep/awk too.
	if [ -n "$UDID" ] && pgrep -f "idevicesyslog.*$UDID" >/dev/null 2>&1; then
		pkill -f "idevicesyslog.*$UDID" && killed=1 || true
	fi
	if [ -s "$OUT" ]; then
		echo "stopped; $OUT: $(wc -l < "$OUT" | tr -d ' ') lines, $(du -h "$OUT" | cut -f1)"
	else
		echo "stopped; $OUT is empty"
	fi
}

case "$ACTION" in
check)
	preflight
	exit 0
	;;
stop)
	stop_capture
	exit 0
	;;
esac

if [ -f "$PIDFILE" ]; then
	echo "already capturing (pid $(cat "$PIDFILE")) -> $OUT" >&2
	echo "stop it first: $0 --stop" >&2
	exit 1
fi

mkdir -p "$(dirname "$OUT")"
cat >&2 <<'EOF'
[*] capturing until stopped. On the iPad, in this order:
      1. open Settings -> Touch ID & Passcode  (ListIdentities on an empty set)
      2. Add a Fingerprint and finish the whole enrolment
      3. unlock once with the new finger       (verify)
    Stop with: scripts/sep/trace-sbio.sh --stop
EOF

case "$MODE" in
syslog)
	idevicesyslog ${UDID:+-u "$UDID"} 2>&1 \
		| grep -E --line-buffered "$FILTER" \
		| awk -v out="$OUT" '{ print; print > out; fflush(out) }' &
	;;
ssh)
	if [ "$("$ROOT/scripts/ipad-ssh.sh" 'id -u' 2>/dev/null || echo 501)" != 0 ]; then
		if ! "$ROOT/scripts/ipad-ssh.sh" 'sudo -n true' 2>/dev/null; then
			echo "ssh log stream needs root; mobile cannot read the kernel os_log." >&2
			echo "Use the default idevicesyslog mode instead." >&2
			exit 1
		fi
		SUDO=sudo
	else
		SUDO=
	fi
	"$ROOT/scripts/ipad-ssh.sh" \
		"$SUDO log stream --style compact --level debug --predicate 'eventMessage CONTAINS \"AppleSEP\" OR eventMessage CONTAINS \"AppleMesa\" OR eventMessage CONTAINS \"IOSEPBiometric\" OR eventMessage CONTAINS \"Catacomb\"'" \
		| awk -v out="$OUT" '{ print; print > out; fflush(out) }' &
	;;
esac

PIPE_PID=$!
echo "$PIPE_PID" > "$PIDFILE"
trap 'rm -f "$PIDFILE"' EXIT
wait "$PIPE_PID" || true
