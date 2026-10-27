#!/bin/busybox sh
# Sourced by the boot-owned initramfs overlay, after USB/network/rescue shells.
# j71t gates both the host (link training) and ANS (controller enable).
# No storage writes. USB rescue must precede either potentially blocking probe.
linux_a9_early_storage() {
  local gate=/sys/module/nvme/parameters/apple_ans_probe
  local host_gate=/sys/module/pcie_apple_h9p/parameters/probe
  local host=/sys/bus/platform/devices/610000000.pcie
  local i=0 limit=${netconsole_wait:-10} d vendor class held=0
  # Also support kernels from round 2, which only have the endpoint gate.
  [ ! -w "$gate" ] || [ "$(cat "$gate")" != N ] || held=1
  [ ! -w "$host_gate" ] || [ "$(cat "$host_gate")" != N ] || held=1
  echo "<5>linux-a9: initramfs storage checkpoint boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)" >/dev/kmsg 2>/dev/null || :
  [ ! -x /linux-a9-pstore ] || /linux-a9-pstore --read
  [ "$held" = 1 ] || return 0
  if [ ! -e /dev/ttyGS0 ] || [ -z "${usb_if:-}" ] ||
     [ -z "$(cat /config/usb_gadget/g1/UDC 2>/dev/null)" ]; then
    echo 'linux-a9: USB diagnostics unavailable; PCIe/ANS remain held (use rescue/DFU)'
    return 1
  fi
  case "$limit" in ''|*[!0-9]*) limit=10 ;; esac
  [ "$limit" -le 30 ] || limit=30
  # Keep serial streaming separate from its getty; telnet is also available.
  # The serial reader can open later: this background writer waits for DTR.
  cat /dev/kmsg >/dev/ttyGS0 2>/dev/null &
  early_kmsg_pid=$!
  while [ "$i" -lt "$limit" ]; do
    start_netconsole
    [ "$(cat /config/netconsole/mac/enabled 2>/dev/null)" = 1 ] && break
    sleep 1
    i=$((i + 1))
  done
  echo 'linux-a9: EARLY USB READY (ACM, ECM/rescue; netconsole if Mac answered); releasing PCIe/ANS'
  [ ! -w "$gate" ] || echo Y >"$gate" || return 1
  [ ! -w "$host_gate" ] || echo Y >"$host_gate" || return 1
  # A synchronous drivers_probe could block on the controller. Leave PID1,
  # USB rescue shells and logging runnable while the existing NVMe worker runs.
  (
    if [ -e "$host" ] && [ ! -L "$host/driver" ]; then
      echo 'linux-a9: reprobe platform PCIe host 610000000.pcie'
      echo 610000000.pcie >/sys/bus/platform/drivers_probe
      echo 'linux-a9: platform PCIe host probe returned'
    fi
    for d in /sys/bus/pci/devices/*; do
      [ -e "$d/vendor" ] || continue
      [ -L "$d/driver" ] && continue
      vendor=$(cat "$d/vendor")
      class=$(cat "$d/class")
      [ "$vendor" = 0x106b ] || continue
      case "$class" in 0x010802|0x018002) ;;
        *) continue ;;
      esac
      echo "linux-a9: reprobe PCI ANS ${d##*/}"
      echo "${d##*/}" >/sys/bus/pci/drivers_probe
    done
    echo 'linux-a9: PCIe/ANS reprobe worker finished'
  ) &
  early_probe_pid=$!
  # Evidence still appears if PID1 is sleeping in ext4_label() or mount().
  # This worker does no block I/O and is canceled at a successful root switch.
  if [ -x /linux-a9-ans-state ]; then
    ( sleep 30; /linux-a9-ans-state ) &
    early_state_pid=$!
  fi
}

linux_a9_storage_status() {
  local f
  echo 'linux-a9: storage timeout; USB rescue remains active'
  for f in /sys/module/pcie_apple_h9p/parameters/probe \
           /sys/module/nvme/parameters/apple_ans_probe \
           /sys/kernel/debug/devices_deferred; do
    [ ! -r "$f" ] || { echo "--- $f"; cat "$f"; }
  done
  if [ -L /sys/bus/platform/devices/610000000.pcie/driver ]; then
    echo 'linux-a9: PCIe host bound or probing (inspect dmesg for completion/link/GPIO/ANS result)'
  else
    echo 'linux-a9: PCIe host unbound or probe still running'
  fi
  ls /sys/bus/pci/devices /sys/class/block 2>/dev/null
  [ ! -x /linux-a9-ans-state ] || /linux-a9-ans-state
}

linux_a9_rescue_evidence() {
  # PID1's stdout can be a logger pipe. Write to the real console as well,
  # independent of UDC/ACM/DTR, and preserve the original pstore files.
  (
    echo 'linux-a9: rescue evidence BEGIN'
    [ ! -x /linux-a9-pstore ] || /linux-a9-pstore --read
    [ ! -x /linux-a9-ans-state ] || /linux-a9-ans-state
    echo 'linux-a9: rescue evidence END'
  ) >/dev/console 2>&1
}

linux_a9_early_storage_stop() {
  # Called only after boot_root verified the mounted root. A broken probe or
  # rescue shell leaves the opt-in kernel deadline armed to retain a panic.
  local deadline=/sys/module/apple_boot_watchdog/parameters/timeout
  if [ -w "$deadline" ] && [ "$(cat "$deadline")" != 0 ]; then
    echo 0 >"$deadline" || return 1
  fi
  [ -z "${early_kmsg_pid:-}" ] || kill "$early_kmsg_pid" 2>/dev/null
  [ -z "${early_state_pid:-}" ] || kill "$early_state_pid" 2>/dev/null
  # Do not kill a potentially uninterruptible PCI probe worker on a failed boot.
  # Normal root transition only occurs after that worker made the disk usable.
  :
}
