#!/usr/bin/env bash
# Orchestrator only: preflight, host console witness, one sysfs trigger, capture.
# Never run as part of a build or an implementation-agent test.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
label=${1:-deferred-cold}
[[ "$label" =~ ^[A-Za-z0-9._-]+$ ]] || exit 2
SSH=${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}
OUT=${AOP_CAPTURE_DIR:-$ROOT/build/periph/aop-stage23/$label-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$OUT"
"$SSH" 'set -eu
  case " $(cat /proc/cmdline) " in *" aop_force_cold=1 "*) ;; *) exit 2;; esac
  case " $(cat /proc/cmdline) " in *" aop_defer=1 "*|*" aop_defer=2 "*) ;; *) exit 2;; esac
  test "$(cat /sys/module/pstore/parameters/backend)" = ramoops
  grep -q "^ramoops" /proc/consoles
  s=$(cat /sys/bus/platform/devices/210800000.aop/cold_release)
  case "$s" in "defer=1 triggered=0"|"defer=2 triggered=0") ;; *) exit 2;; esac
  printf "%s\n" "$s"
  cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline
  grep ramoops /proc/consoles
  for p in mem_address mem_size console_size record_size; do
    printf "%s=" "$p"; sudo -n cat "/sys/module/ramoops/parameters/$p"
  done
  sudo -n cat /sys/kernel/debug/apple-a9-aop-active/state
  sudo -n dmesg' > "$OUT/pre-trigger.txt" 2> "$OUT/pre-trigger.err"
"$SSH" 'sudo -n cat /sys/firmware/fdt' > "$OUT/pre-trigger.dtb"
"$SSH" 'sudo -n cat /proc/iomem' > "$OUT/pre-trigger-iomem.txt"
python3 -B scripts/periph/aop/check-stage23-memory.py --runtime \
    --dtb "$OUT/pre-trigger.dtb" --iomem "$OUT/pre-trigger-iomem.txt" \
    > "$OUT/pre-trigger-memory-check.txt"
"$SSH" 'sudo -n true && printf "AOP live-console-ready\n" && sudo -n dmesg --follow --raw' \
    > "$OUT/console-live.txt" 2> "$OUT/console-live.err" &
console_pid=$!
cleanup() {
    kill "$console_pid" 2>/dev/null || true
    wait "$console_pid" 2>/dev/null || true
}
trap cleanup EXIT
sleep 2
kill -0 "$console_pid" # Refuse release if the host console witness already died.
rg -q '^AOP live-console-ready$' "$OUT/console-live.txt"
printf 'capture=%s\ntrigger UTC=%s\n' "$OUT" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee "$OUT/trigger-host.txt"
# The store queues work and returns immediately; it never waits for firmware.
"$SSH" 'printf "1\n" | sudo -n tee /sys/bus/platform/devices/210800000.aop/cold_release' \
    > "$OUT/trigger.txt" 2> "$OUT/trigger.err" || true
AOP_CAPTURE_DIR="$OUT" AOP_CAPTURE_WAIT_SECONDS=60 \
    scripts/periph/aop/capture-stage23.sh "$label"
