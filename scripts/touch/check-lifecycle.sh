#!/bin/sh
# OWNER-RUN ON THE IPAD ONLY. One explicit cycle, no recovery loop.
# Modes: N1_TEST=cycle (default), missing-fw, missing-cal.
# Missing-file modes temporarily hide one file in the RAM rootfs, restore
# it on exit/signals, then perform one normal rebind. Never use factory data.
set -eu
mode=${N1_TEST:-cycle}
case "$mode" in cycle|missing-fw|missing-cal) ;; *) echo 'invalid N1_TEST' >&2; exit 2 ;; esac
driver=/sys/bus/spi/drivers/apple-n1
device=
for p in "$driver"/spi*; do
    test -e "$p" || continue
    test -z "$device" || { echo 'More than one N1 is bound' >&2; exit 1; }
    device=${p##*/}
done
test -n "$device" || { echo 'N1 is not bound; capture the boot/probe error first' >&2; exit 1; }
test "$(cat /sys/module/apple_n1/parameters/boot_stage)" = 3 || {
    echo 'Expected boot_stage=3; this test does not change stage parameters' >&2; exit 1;
}
find_event() {
    for p in /sys/class/input/event*; do
        test -r "$p/device/name" || continue
        if test "$(cat "$p/device/name")" = 'Apple N1 Touchscreen'; then
            printf '/dev/input/%s\n' "${p##*/}"
            return 0
        fi
    done
    return 1
}
event=$(find_event)
printf 'mode=%s device=%s event=%s boot_stage=3\n' "$mode" "$device" "$event"
cat /proc/cmdline
printf 'INPUT_PROP bitmap: '; cat "/sys/class/input/${event##*/}/device/properties"
if test -x /tmp/n1-input-info; then /tmp/n1-input-info "$event"; fi
original=
hidden=
restore_file() {
    if test -n "$hidden" && test -f "$hidden"; then mv "$hidden" "$original"; fi
}
trap restore_file EXIT
trap 'exit 130' HUP INT TERM
# Hold a finger down during unbind when checking release/teardown behavior.
timeout 30 sh -ec 'printf "%s\n" "$2" >"$1/unbind"' sh "$driver" "$device"
if find_event >/dev/null; then echo 'Touch event node survived unbind' >&2; exit 1; fi
printf 'PASS: unbind completed; N1 event node removed\n'
if test "$mode" != cycle; then
    case "$mode" in
        missing-fw) original=/lib/firmware/apple/j71t-n1.bin ;;
        missing-cal) original=/lib/firmware/apple/j71t-n1-calibration.bin ;;
    esac
    hidden="$original.n1-test-hidden"
    test -f "$original" && test ! -e "$hidden"
    mv "$original" "$hidden"
    before=$(cut -d ' ' -f1 /proc/uptime)
    status=0
    timeout 5 sh -ec 'printf "%s\n" "$2" >"$1/bind"' sh "$driver" "$device" || status=$?
    after=$(cut -d ' ' -f1 /proc/uptime)
    restore_file
    hidden=
    printf 'Missing file bind exit=%s uptime-before=%s uptime-after=%s\n' "$status" "$before" "$after"
    case "$status" in
        1|2) ;; # shell reports the expected failed sysfs write
        *) echo 'FAIL: missing-file probe succeeded or timed out/terminated' >&2; exit 1 ;;
    esac
    test ! -e "$driver/$device" || { echo 'Unexpected bound N1 after missing-file failure' >&2; exit 1; }
fi
timeout 30 sh -ec 'printf "%s\n" "$2" >"$1/bind"' sh "$driver" "$device"
event=$(find_event)
printf 'PASS: normal rebind restored %s\n' "$event"
if test -x /tmp/n1-input-info; then /tmp/n1-input-info "$event"; fi
dmesg | grep -Ei 'apple-n1|timeout|stall|lockup' || true
