#!/bin/sh
# Owner-run on Linux via scripts/linux-run.sh -f; ten-second evdev sample.
set -eu
event=
for node in /sys/class/input/event*; do
    test -r "$node/device/name" || continue
    if test "$(cat "$node/device/name")" = 'Apple N1 Touchscreen'; then
        event="/dev/input/${node##*/}"
        break
    fi
done
test -n "$event" || { echo 'Apple N1 evdev node absent' >&2; exit 1; }
test -c "$event" || { echo "Missing character device $event" >&2; exit 1; }
echo "Touch and lift one or more fingers now; sampling $event for at most ten seconds"
# Reading a pipe may produce short blocks. count is deliberately omitted;
# timeout is the bound and the result stays in RAM. timeout may return124.
set +e
timeout 10 dd if="$event" of=/tmp/n1-events.bin bs=24 2>/tmp/n1-dd.log
status=$?
set -e
case "$status" in 0|124|143) ;; *) cat /tmp/n1-dd.log; exit "$status" ;; esac
size=$(wc -c </tmp/n1-events.bin)
test "$size" -gt 0 || { echo 'No input events received' >&2; exit 1; }
test $((size % 24)) -eq 0 || { echo 'Partial arm64 input_event record' >&2; exit 1; }
echo "$size bytes; each arm64 event is 24 bytes: time 16, type 2, code 2, value 4 (little-endian)"
# Skip the 16-byte timeval in each record, then print four 16-bit words.
# These are TYPE CODE VALUE_LO VALUE_HI, one event per line.
offset=16
shown=0
while test "$offset" -lt "$size" && test "$shown" -lt 128; do
    if test "$shown" -eq 64 && test "$size" -gt 3072; then
        echo '... last 64 events ...'
        offset=$((size - 64 * 24 + 16))
    fi
    dd if=/tmp/n1-events.bin bs=1 skip="$offset" count=8 2>/dev/null | od -An -t x2
    offset=$((offset + 24))
    shown=$((shown + 1))
done
