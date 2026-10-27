#!/bin/busybox sh
# Sourced only by PID1 after an explicit rescue continue. The kernel gate
# accepts only a port that has never enumerated; it refuses live NVMe roots.
linux_a9_rescue_retry_ans() {
  local deadline=/sys/module/apple_boot_watchdog/parameters/timeout
  local recover=/sys/bus/platform/devices/610000000.pcie/ans_recover
  [ "${root_mode:-}" = nvme ] || return 0
  [ -r "$recover" ] && [ -w "$recover" ] || return 0
  case "$(cat "$recover")" in eligible=1\ *) ;;
    *) echo 'linux-a9: deep ANS retry ineligible (port already published or non-link failure)'; return 0 ;;
  esac
  # Explicit continue starts a fresh, bounded 120-second recovery window.
  [ ! -w "$deadline" ] || echo 120 >"$deadline" || return 1
  echo 'linux-a9: explicit rescue continue: one deep ANS link retry'
  # Keep PID1 and its USB shells runnable even if an MMIO transaction wedges.
  ( echo 1 >"$recover"; rc=$?
    echo "linux-a9: deep ANS rescue worker returned $rc; $(cat "$recover")"
  ) &
}
