#!/usr/bin/env bash
# Dump the A9 SEP mailbox state from the running iPad (Omarchy, Linux side).
#
# The driver is built into tri53-sep to -sep5 (and the `sep` workstream) and
# is inert unless the kernel command line has sep_a9.enable=1. See
# docs/sep/linux-live-test.md for the full test sequence.
#
# Without --arm nothing touches the SEP registers: the script prints the
# preconditions (reservation, power domain, probe line) and the driver's
# software state. --arm refuses unless pm_genpd_summary shows "sep on", then
# arms the mailbox (debugfs start: on tri53-sep2 the wrapper activation, then
# drain and record). Nothing is ever sent by this script. Arm only once sepOS
# is known to be running.
#
# Usage: scripts/sep/sep-dump.sh [--arm]
#   OMARCHY_SSH overrides the ssh wrapper (default scripts/omarchy/ssh.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH="${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}"
ARM=0
[ "${1:-}" = --arm ] && { ARM=1; shift; }
[ $# -eq 0 ] || { sed -n '2,16p' "$0" >&2; exit 2; }

# shellcheck disable=SC2016
"$SSH" "sudo -n env ARM=$ARM sh -s" <<'EOF'
set -u
D=/sys/kernel/debug/sep-a9
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug
echo "== cmdline"; tr ' ' '\n' </proc/cmdline | grep -E '^(linux_a9.entry|sep_a9\.)'
echo "== reservation"; dmesg | grep -E 'reserved mem:.*sep-shmem' || echo "MISSING: sep-shmem@87adb0000 not reserved"
grep -i '87adb0000' /proc/iomem || true
echo "== power domain"
PD=$(grep -E '^sep ' /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>/dev/null)
echo "${PD:-(no genpd line for sep)}"
echo "== pmgr"; dmesg | grep -E 'PMGR sep ' | tail -8 || true
echo "== probe"; dmesg | grep -E 'sep-a9' | tail -40
if [ ! -d "$D" ]; then
	echo "no $D: boot an entry with sep_a9.enable=1" >&2
	exit 1
fi
if [ "$ARM" = 1 ]; then
	set -- $PD
	if [ "${2:-}" != on ]; then
		echo "REFUSED: sep power domain not on; not arming" >&2
		exit 1
	fi
	echo "== arming (wrapper activation + drain, nothing sent)"
	if ! echo 1 >"$D/start"; then
		echo "ARM FAILED: see the sep-a9 arm lines below" >&2
		dmesg | grep -E 'sep-a9.*arm' | tail -5
		exit 1
	fi
	sleep 1
	dmesg | grep -E 'sep-a9.*arm' | tail -8
fi
echo "== status";    cat "$D/status"
if [ -e "$D/fifo" ]; then
	echo "== fifo";  cat "$D/fifo"
fi
if [ -e "$D/wake-test" ]; then
	echo "== wake-test"; cat "$D/wake-test"
fi
echo "== endpoints"; cat "$D/endpoints"
echo "== log";       cat "$D/log"
echo "== xfer";      cat "$D/xfer"
EOF
