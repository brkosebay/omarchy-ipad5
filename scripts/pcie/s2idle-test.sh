#!/bin/sh
# One suspend-to-idle attempt on the iPad, with before/after state and the
# kernel log of the attempt. Runs ON the device as root (Omarchy or any
# BusyBox/systemd userspace); see docs/pcie/suspend-test-plan.md.
#
#   s2idle-test.sh MODE PM_TEST [KEEP_GADGET]
#     MODE         pcie_apple_h9p.suspend for this attempt (0..3)
#     PM_TEST      none | devices | platform  (/sys/power/pm_test; with
#                  devices/platform the kernel waits 5 s and resumes by itself,
#                  "none" really sleeps and needs a wake: the Home button)
#     KEEP_GADGET  dwc2.s2idle_keep_gadget (default 1: keep the USB link up)
#
# Output: /run/s2idle/<time>-m<MODE>-<PM_TEST>.txt and /run/s2idle/last.txt.
# Both knobs go back to 0 / none afterwards, so nothing can suspend later
# by accident.
set -u

MODE="${1:?MODE 0..3}"
TEST="${2:?PM_TEST none|devices|platform}"
KEEP="${3:-1}"
H9P=/sys/module/pcie_apple_h9p/parameters/suspend
DWC2=/sys/module/dwc2/parameters/s2idle_keep_gadget
OUT=/run/s2idle
mkdir -p "$OUT"
LOG="$OUT/$(date +%H%M%S)-m$MODE-$TEST.txt"

snap() {
	echo "== state $1"
	for d in /sys/bus/pci/devices/*; do
		[ -e "$d" ] || continue
		echo "pci $(basename "$d") $(cat "$d/vendor"):$(cat "$d/device") link=$(cat "$d/current_link_speed" 2>/dev/null)/x$(cat "$d/current_link_width" 2>/dev/null) driver=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)"
	done
	for d in /sys/class/nvme/nvme*; do
		[ -e "$d" ] && echo "nvme $(basename "$d") state=$(cat "$d/state")"
	done
	ip -br link 2>/dev/null
	iw dev 2>/dev/null | grep -E "Interface|ssid|channel"
	grep -E "^(pcie|gpio|usb|aic|rtmux|disp0|dp |mipi_dsi|dwi)" /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>/dev/null
}

{
	echo "kernel: $(uname -r) $(cat /proc/cmdline)"
	echo "mode=$MODE pm_test=$TEST keep_gadget=$KEEP"
	echo "mem_sleep: $(cat /sys/power/mem_sleep) state: $(cat /sys/power/state)"
	snap before
} > "$LOG" 2>&1

echo 1 > /sys/power/pm_print_times
echo 1 > /sys/power/pm_debug_messages
echo "$KEEP" > "$DWC2"
echo "$MODE" > "$H9P"
echo "$TEST" > /sys/power/pm_test
echo "s2idle-test: start mode=$MODE pm_test=$TEST keep=$KEEP" > /dev/kmsg
sync

t0=$(date +%s)
echo freeze > /sys/power/state
rc=$?
t1=$(date +%s)

echo none > /sys/power/pm_test
echo 0 > "$H9P"
echo 0 > "$DWC2"
echo "s2idle-test: back rc=$rc after $((t1 - t0)) s" > /dev/kmsg

# Let NVMe's reset work and the Wi-Fi reconnect finish
sleep 20

{
	echo "== result rc=$rc elapsed=$((t1 - t0))s"
	snap after
	echo "== nvme read"
	if [ -b /dev/nvme0n1 ]; then
		dd if=/dev/nvme0n1 of=/dev/null bs=1M count=64 iflag=direct 2>&1 | tail -1
	fi
	echo "== suspend_stats"
	grep -r . /sys/power/suspend_stats/ 2>/dev/null
	echo "== kernel log of this attempt"
	dmesg | awk '/s2idle-test: start/ { buf = "" } { buf = buf $0 "\n" } END { printf "%s", buf }'
} >> "$LOG" 2>&1

ln -sf "$LOG" "$OUT/last.txt"
echo "$LOG"
