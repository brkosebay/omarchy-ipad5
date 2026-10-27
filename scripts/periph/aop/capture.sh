#!/usr/bin/env bash
# ORCHESTRATOR ONLY. Capture on Linux; no mounting, PM or boot changes.
# Usage: capture.sh cold|warm|preflight|active
# AOP_CAPTURE_ROUND=aop-r5 selects the Round 5 active capture directory.
#
# Debugfs paths use HYPHENS: /sys/kernel/debug/apple-a9-aop/ (observer) and
# /sys/kernel/debug/apple-a9-aop-active/ (active driver). The round-2
# r2-*-capture.txt files were taken by hand with the underscore spelling
# (the module-parameter prefix apple_a9_aop), which never exists; a missing
# directory is now reported explicitly instead of looking like empty data.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
LABEL="${1:?expected cold, warm, preflight or active}"
case "$LABEL" in cold|warm|preflight) ROUND=aop-r2 ;; active) ROUND=${AOP_CAPTURE_ROUND:-aop-r4} ;; *) exit 2 ;; esac
[[ "$ROUND" =~ ^aop-r[0-9]+$ ]] || { echo 'invalid AOP_CAPTURE_ROUND' >&2; exit 2; }
SSH="$ROOT/scripts/omarchy/ssh.sh"
OUT="$ROOT/build/periph/$ROUND/live/$LABEL-$(date -u +%Y%m%dT%H%M%SZ)"
OBS=/sys/kernel/debug/apple-a9-aop
ACT=/sys/kernel/debug/apple-a9-aop-active
mkdir -p "$OUT"
"$SSH" 'tr "\0" "\n" </proc/device-tree/compatible | grep -Fx apple,j71t' > "$OUT/board.txt"
"$SSH" 'uname -a; cat /proc/sys/kernel/random/boot_id /proc/uptime /proc/cmdline; if test -e /proc/device-tree/chosen/linux,booted-from-kexec; then echo kexec_marker=present; else echo kexec_marker=absent; fi; findmnt /; cat /sys/devices/system/cpu/online' > "$OUT/boot.txt"
"$SSH" 'sudo -n dmesg' > "$OUT/dmesg.txt"
"$SSH" 'sudo -n cat /sys/kernel/debug/pm_genpd/pm_genpd_summary' > "$OUT/genpd-before.txt"
if ! "$SSH" "sudo -n test -d $OBS"; then
    echo "$OBS missing: wrong build/DT/board, apple_a9_aop.debug=0, or observer probe failure." >&2
    echo "See $OUT/boot.txt (cmdline) and $OUT/dmesg.txt; stop this pass." >&2
    exit 2
fi
"$SSH" "sudo -n cat $OBS/status" > "$OUT/status.txt"
if [ "$LABEL" = active ]; then
    if ! "$SSH" "sudo -n test -d $ACT"; then
        echo "$ACT missing: CONFIG_APPLE_A9_AOP_ACTIVE off, active/debug parameter off, or probe failure." >&2
        grep -i 'aop' "$OUT/dmesg.txt" >&2 || true
        exit 2
    fi
    for f in state transcript rings patchbay; do
        "$SSH" "sudo -n cat $ACT/$f" > "$OUT/active-$f.txt"
    done
    if "$SSH" "sudo -n test -r $ACT/fingerprint"; then
        "$SSH" "sudo -n cat $ACT/fingerprint" > "$OUT/active-fingerprint.txt"
        if grep -q '^layout supported=' "$OUT/active-state.txt" &&
           ! grep -qx 'snapshot_end=1' "$OUT/active-fingerprint.txt"; then
            echo "Incomplete Round 7 fingerprint capture: $OUT/active-fingerprint.txt" >&2
            exit 2
        fi
    else
        echo 'fingerprint diagnostics unavailable on this kernel' > "$OUT/active-fingerprint.txt"
        if grep -q '^layout supported=' "$OUT/active-state.txt"; then
            echo "Round 7 fingerprint missing: $OUT/active-fingerprint.txt" >&2
            exit 2
        fi
    fi
    "$SSH" 'for d in /sys/bus/iio/devices/iio:device*; do
        [ "$(cat "$d/name" 2>/dev/null)" = apple-a9-aop-accel ] || continue
        echo "== $d"; ls "$d"
        for f in in_accel_x_raw in_accel_y_raw in_accel_z_raw in_accel_scale \
                 in_accel_sampling_frequency in_accel_mount_matrix; do
            printf "%s=" "$f"; cat "$d/$f" 2>&1
        done
    done; ls /sys/bus/iio/devices/' > "$OUT/iio.txt" || true
fi
"$SSH" 'sudo -n cat /sys/kernel/debug/pm_genpd/pm_genpd_summary' > "$OUT/genpd-after.txt"
"$SSH" 'cat /proc/interrupts; sudo -n cat /proc/iomem' > "$OUT/interrupts-iomem.txt"
"$SSH" 'sudo -n cat /sys/firmware/fdt' > "$OUT/linux.dtb"
"$SSH" 'tar -C /sys/firmware/devicetree/base -cf - .' > "$OUT/linux-dt-fs.tar"
if [ "$LABEL" = preflight ]; then
    "$SSH" "sudo -n cat $OBS/preflight" > "$OUT/preflight.txt"
fi
echo "$OUT"
cat "$OUT/status.txt"
[ "$LABEL" != preflight ] || cat "$OUT/preflight.txt"
if [ "$LABEL" = active ]; then
    head -n 3 "$OUT/active-state.txt"
    cat "$OUT/iio.txt"
    exit 0
fi
grep -qx 'snapshot_errno=0' "$OUT/status.txt" || {
    echo 'Wrapper reads skipped; retain power-state evidence and stop this pass.' >&2
    exit 2
}
grep -qx 'inbox_faults=0 outbox_faults=0' "$OUT/status.txt" || {
    echo 'Mailbox fault flags present; stop without acknowledging/resetting.' >&2
    exit 2
}
