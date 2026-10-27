#!/bin/sh
# Read-only telemetry, run ON Linux by orchestrator. Fresh output directory.
# Usage: sh linux-sample.sh /path/to/new-output [samples (1..720)]
set -eu
umask 077
out=${1:?fresh output directory required}
count=${2:-60}
case "$count" in ''|*[!0-9]*) exit 2;; esac
[ "$count" -ge 1 ] && [ "$count" -le 720 ] || exit 2
mkdir "$out"
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sh "$base/linux-observe.sh" > "$out/before.txt"
printf 'epoch\tpath\tvalue\n' > "$out/samples.tsv"
i=0
while [ "$i" -lt "$count" ]; do
    stamp=$(date +%s)
    for path in /sys/class/power_supply/*/current_now \
        /sys/class/power_supply/*/voltage_now /sys/class/power_supply/*/status \
        /sys/class/power_supply/*/temp /sys/class/power_supply/*/capacity \
        /sys/class/thermal/thermal_zone*/temp; do
        [ -r "$path" ] || continue
        if value=$(cat "$path" 2>/dev/null); then
            printf '%s\t%s\t%s\n' "$stamp" "$path" "$value"
        else
            printf '%s\t%s\tUNAVAILABLE\n' "$stamp" "$path"
        fi
    done >> "$out/samples.tsv"
    i=$((i + 1))
    sleep 5
done
sh "$base/linux-observe.sh" > "$out/after.txt"
