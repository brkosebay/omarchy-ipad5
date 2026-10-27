#!/usr/bin/env bash
# Orchestrator-only read-only collection. This script never starts an attempt.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
label=${1:-stage23}
[[ "$label" =~ ^[A-Za-z0-9._-]+$ ]] || exit 2
SSH=${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}
OUT=${AOP_CAPTURE_DIR:-$ROOT/build/periph/aop-stage23/$label-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$OUT"
capture_wait=${AOP_CAPTURE_WAIT_SECONDS:-60}
capture_hold=${AOP_CAPTURE_HOLD_SECONDS:-0}
[[ "$capture_wait" =~ ^[0-9]+$ ]] && [ "$capture_wait" -ge 1 ] && [ "$capture_wait" -le 240 ] || exit 2
[[ "$capture_hold" =~ ^[0-9]+$ ]] && [ "$capture_hold" -le 240 ] || exit 2
# Wait for the single boot-time attempt to finish; reading state never retries it.
"$SSH" 'for i in $(seq 1 '"$capture_wait"'); do
  s=$(sudo -n head -n1 /sys/kernel/debug/apple-a9-aop-active/state) || exit 1
  printf "%s\n" "$s"
  case "$s" in *phase=streaming*|*phase=system-ready*|*phase=stopped*|*phase=failed*|*phase=refused*) break;; esac
  sleep 1
done' > "$OUT/progress.txt" 2>&1 || true
if [ "$capture_hold" -gt 0 ]; then
    "$SSH" 'sudo -n cat /sys/kernel/debug/apple-a9-aop-active/state' \
        > "$OUT/ready-state.txt" 2>&1 || true
    # trigger-cold keeps the live host dmesg witness running throughout this hold.
    sleep "$capture_hold"
fi
"$SSH" 'cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline' > "$OUT/identity.txt" 2>&1 || true
for name in state registers startup_transcript transcript rings samples cold_memory patchbay system_buffers previous_shutdown; do
    "$SSH" "sudo -n cat /sys/kernel/debug/apple-a9-aop-active/$name" \
        > "$OUT/$name.txt" 2> "$OUT/$name.err" || true
done
for name in cold_before cold_after crashlog; do
    "$SSH" "sudo -n cat /sys/kernel/debug/apple-a9-aop-active/$name" \
        > "$OUT/$name.bin" 2> "$OUT/$name.err" || true
done
# Full frozen RTKit crashlog (not the 256-byte dmesg prefix), decoded on the host.
if [ -s "$OUT/crashlog.bin" ]; then
    python3 -B scripts/periph/aop/decode-crashlog.py "$OUT/crashlog.bin" \
        > "$OUT/crashlog.txt" 2>&1 || true
fi
"$SSH" 'cat /sys/bus/platform/devices/210800000.aop/handoff 2>/dev/null; cat /sys/bus/platform/devices/210800000.aop/cold_release' \
    > "$OUT/handoff.txt" 2>&1 || true
if [ -s "$OUT/cold_before.bin" ] && [ -s "$OUT/cold_after.bin" ]; then
    inspect_args=()
    fw=build/periph/aop-r2/firmware/apple/aop
    if [ -f "$fw/ipad6faop-20H350.bin" ] && [ -f "$fw/ipad6faop_ext-20H350.bin" ]; then
        inspect_args+=(--firmware "$fw/ipad6faop-20H350.bin" --ext "$fw/ipad6faop_ext-20H350.bin")
    fi
    python3 -B scripts/periph/aop/inspect-cold-memory.py "$OUT" "${inspect_args[@]}" \
        > "$OUT/cold-memory-report.txt" 2>&1 || true
fi
"$SSH" 'sudo -n cat /sys/firmware/fdt' > "$OUT/running.dtb" 2> "$OUT/fdt.err" || true
"$SSH" 'sudo -n cat /proc/iomem' > "$OUT/iomem.txt" 2>&1 || true
"$SSH" 'for d in /sys/bus/iio/devices/iio:device*; do
  [ -r "$d/name" ] || continue
  case "$(cat "$d/name")" in *aop*|*AOP*)
    printf "%s\n" "$d"
    for f in name in_accel_x_raw in_accel_y_raw in_accel_z_raw in_accel_scale in_accel_sampling_frequency; do
      printf "%s=" "$f"; cat "$d/$f" 2>&1 || true
    done;;
  esac
done' > "$OUT/iio.txt" 2>&1 || true
"$SSH" 'sudo -n dmesg' > "$OUT/dmesg.txt" 2>&1 || true
python3 -B scripts/periph/aop/check-stage23-memory.py --runtime \
    --dtb "$OUT/running.dtb" --iomem "$OUT/iomem.txt" \
    > "$OUT/memory-check.txt" 2>&1 || true
printf 'capture=%s\n' "$OUT"
cat "$OUT/state.txt"
cat "$OUT/memory-check.txt"
