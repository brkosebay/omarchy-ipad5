#!/bin/sh
# Runtime status of every existing device. No policy writes or device access.
set -eu
printf 'device\tdriver\tcontrol\tstatus\tusage\tactive_ms\tsuspended_ms\n'
find /sys/devices -name runtime_status -type f | sort | while IFS= read -r file; do
    power=${file%/runtime_status}
    dev=${power%/power}
    driver=$(readlink "$dev/driver" 2>/dev/null || :)
    printf '%s\t%s' "$dev" "${driver##*/}"
    for key in control runtime_status runtime_usage runtime_active_time runtime_suspended_time; do
        value=$(cat "$power/$key" 2>/dev/null || printf NA)
        printf '\t%s' "$value"
    done
    printf '\n'
done
