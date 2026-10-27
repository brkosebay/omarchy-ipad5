#!/usr/bin/env bash
# ORCHESTRATOR ONLY, runs on the iPad as root. Never invoked by an offline
# builder. docs/power/smart-cover-test-plan.md, steps D1-D4.
#
#   smart-cover-sleep.sh suspend LABEL   systemctl suspend now (the path logind
#                                        and ipad-sleep take), log until resumed
#   smart-cover-sleep.sh wait LABEL      do not suspend: wait up to 120 s for the
#                                        cover (logind) to do it, log until resumed
#
# LABEL only names the log: /run/smart-cover-r1/<time>-<LABEL>.log, linked as
# last.log. The log has the cover IRQ/wake state before and after, the
# elapsed sleep, the wake IRQ and the PM/driver kernel lines of the cycle.
set -euo pipefail
MODE=${1:?suspend or wait} LABEL=${2:?label}
case "$MODE" in suspend|wait) ;; *) echo 'mode: suspend or wait' >&2; exit 2 ;; esac
[[ "$LABEL" =~ ^[a-z0-9-]+$ ]] || { echo 'invalid label' >&2; exit 2; }
[ "$(id -u)" = 0 ]
mkdir -p /run/smart-cover-r1
LOG=/run/smart-cover-r1/$(date -u +%Y%m%dT%H%M%SZ)-$LABEL.log
ln -sfn "$LOG" /run/smart-cover-r1/last.log
exec > "$LOG" 2>&1

lid_node() {
	local p
	for p in /sys/class/input/event*/device/name; do
		[ "$(cat "$p")" = "iPad Smart Cover" ] || continue
		p=${p%/device/name}
		echo "/dev/input/${p##*/}"
		return
	done
}

snap() {
	echo "== snapshot $1 $(cut -d' ' -f1 /proc/uptime)"
	uname -r
	cat /sys/power/mem_sleep
	grep -E 'd2365|Smart Cover|gpio-keys' /proc/interrupts || true
	for f in /sys/power/suspend_stats/{success,fail,last_failed_dev,last_failed_errno} \
		 /sys/power/pm_wakeup_irq; do
		[ ! -r "$f" ] || printf '%s=%s\n' "$f" "$(cat "$f")"
	done
	grep -E '^name|3c|smart-cover|gpio-keys' /sys/kernel/debug/wakeup_sources 2>/dev/null || true
	for d in /sys/devices/platform/smart-cover /sys/devices/platform/smart-cover-aux; do
		[ ! -r "$d/power/wakeup" ] || printf '%s/power/wakeup=%s\n' "$d" "$(cat "$d/power/wakeup")"
	done
	node=$(lid_node)
	if [ -n "$node" ] && command -v evtest >/dev/null; then
		if evtest --query "$node" EV_SW SW_LID; then echo "SW_LID=0 (open)"
		else echo "SW_LID=1 (closed) rc=$?"; fi
	fi
}

echo 1 > /sys/power/pm_print_times
echo 1 > /sys/power/pm_debug_messages
snap before
before=$(cat /sys/power/suspend_stats/success)
echo "smart-cover-r1 START $MODE $LABEL" > /dev/kmsg
start=$(date +%s)
if [ "$MODE" = suspend ]; then
	systemctl suspend
fi
# Wait for the sleep to be entered and left (logind may need the cover).
limit=120
[ "$MODE" = wait ] || limit=900
until [ "$(cat /sys/power/suspend_stats/success)" != "$before" ] ||
      [ $(( $(date +%s) - start )) -ge $limit ]; do
	sleep 1
done
elapsed=$(( $(date +%s) - start ))
sleep 5	# let gpio-keys and the event thread settle after resume
echo "smart-cover-r1 RETURN $MODE $LABEL elapsed=${elapsed}s" > /dev/kmsg
if [ "$(cat /sys/power/suspend_stats/success)" = "$before" ]; then
	echo "result NO-SLEEP elapsed=${elapsed}s"
else
	echo "result slept elapsed=${elapsed}s"
fi
snap after
echo "== kernel"
dmesg | sed -n '/smart-cover-r1 START '"$MODE $LABEL"'/,$p' |
	grep -E 'smart-cover-r1|PM: (suspend|Triggering|resume|Wakeup|wakeup)|d2365|hall|gpio-keys|Smart Cover|PM: .*abort' || true
