#!/bin/sh
# OWNER-RUN ON THE IPAD ONLY, after booting the corrected kernel at stage1:
# TIMEOUT=45 scripts/linux-run.sh -f scripts/touch/capture-stage2.sh
# The touch implementation agent must never execute this script on a device.
set -eu

driver=/sys/bus/spi/drivers/apple-n1
device=
for path in "$driver"/spi*; do
    test -e "$path" || continue
    if test -n "$device"; then
        echo 'More than one N1 is bound; select the device explicitly.' >&2
        exit 1
    fi
    device=${path##*/}
done
if test -z "$device"; then
    echo 'N1 is not bound. Capture the stage1 error before attempting stage2.' >&2
    exit 1
fi
printf 'N1 stage2: device=%s; one bounded rebind, control tracing enabled\n' "$device"
echo 1 >/sys/module/apple_n1/parameters/trace
echo 2 >/sys/module/apple_n1/parameters/boot_stage
result=0
timeout 30 sh -ec '
    printf "%s\n" "$2" >"$1/unbind"
    printf "%s\n" "$2" >"$1/bind"
' sh "$driver" "$device" || result=$?
# Preserve the first error and earlier execute/wake exchanges: no tail truncation.
dmesg | grep -Ei 'apple-n1|apple-s8000-spi|timeout|stall|lockup' || true
printf 'N1 stage2 rebind exit=%s\n' "$result"
exit "$result"
