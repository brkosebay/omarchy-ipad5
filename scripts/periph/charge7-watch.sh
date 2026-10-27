#!/bin/sh
# Linux-side, read-only bounded logger. Run locally on the iPad, so unplugging
# its USB network cannot stop logging. Default 30 minutes, maximum one hour.
set -eu
seconds=${WATCH_SECONDS:-1800}
case "$seconds" in ''|*[!0-9]*) echo 'WATCH_SECONDS must be 1..3600' >&2; exit 2;; esac
[ "$seconds" -ge 1 ] && [ "$seconds" -le 3600 ] || exit 2
command -v timeout >/dev/null
[ -r /proc/uptime ] || { echo 'Run this logger on Linux' >&2; exit 2; }
if [ "${CHARGE7_BOUNDED:-}" != 1 ]; then
    export CHARGE7_BOUNDED=1
    exec timeout --signal=TERM --kill-after=2s "${seconds}s" sh "$0"
fi
ps=${SYSFS_POWER_SUPPLY:-/sys/class/power_supply}
read elapsed rest < /proc/uptime
start=${elapsed%.*}
stop=$((start + seconds))
next=$start
read_prop() {
    timeout --signal=TERM --kill-after=0.2s 0.5s cat "$ps/$1/$2" 2>/dev/null || printf 'err'
}
printf 'utc\tuptime_s\tonline\tusb_type\tcurrent_max_ua\tinput_limit_ua\tbattery_current_ua\tbattery_voltage_uv\tbattery_temp_dC\tbattery_status\treason\n'
while :; do
    read elapsed rest < /proc/uptime
    now=${elapsed%.*}
    [ "$now" -lt "$stop" ] || break
    if [ "$now" -lt "$next" ]; then sleep 0.1; continue; fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$elapsed" \
        "$(read_prop usb online)" "$(read_prop usb usb_type)" \
        "$(read_prop usb current_max)" "$(read_prop charger input_current_limit)" \
        "$(read_prop battery current_now)" "$(read_prop battery voltage_now)" \
        "$(read_prop battery temp)" "$(read_prop battery status)" \
        "$(read_prop charger charge7_state)"
    next=$((now + 1))
done
