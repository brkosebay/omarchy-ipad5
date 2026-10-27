#!/usr/bin/env bash
# Runs ON THE IPAD as root (installed and started by usb-host5-watch.sh; the
# orchestrator is the only one who runs that). Event logger for the usb-host5
# camera-adapter test: it never changes the USB role, the Tristar or the PMU.
#
# Every second it reads the kernel's Lightning policy state
# (/sys/bus/i2c/devices/0-001a/lightning_state). When the phase, role, ID,
# handshake or charger state changes it writes a numbered snapshot (state,
# role-switch registers, policy log, lsusb, lsblk, power supplies, kernel log)
# and one timeline line. When a USB disk partition appears it mounts it
# READ-ONLY on a private directory, records a listing, and unmounts it.
# No deadline: it runs until stopped (touch $OUT/stop or stop the unit),
# capped at MAX_HOURS (default 12) only as a runaway guard.
set -uo pipefail
OUT=${OUT:-/var/log/linux-a9-usb-host5}
STATE=/sys/bus/i2c/devices/0-001a/lightning_state
REGS=/sys/kernel/debug/apple-usb-role/regs
AUTOLOG=/sys/kernel/debug/apple-tristar/auto_log
ROLE=/sys/class/usb_role/lightning-role-switch/role
MNT=/run/linux-a9-usb-host5/mnt
MAX_HOURS=${MAX_HOURS:-12}

mkdir -p "$OUT" "$MNT"
rm -f "$OUT/stop"
run_id=$(date -u +%Y%m%dT%H%M%SZ)
RUN="$OUT/$run_id"
mkdir -p "$RUN"
ln -sfn "$RUN" "$OUT/latest"
TL="$RUN/timeline.txt"
end=$(( $(date +%s) + MAX_HOURS * 3600 ))
n=0
prev_key=""
declare -A mounted_seen=()

stamp() { date -u +%H:%M:%S.%3N; }
note() { printf '%s %s\n' "$(stamp)" "$*" | tee -a "$TL"; }

field() { sed -n "s/^$1: //p" "$2" | head -1; }

snapshot() {
	local why=$1 dir
	n=$((n + 1))
	dir=$(printf '%s/%03d-%s' "$RUN" "$n" "$why")
	mkdir -p "$dir"
	cat "$STATE" > "$dir/lightning_state.txt" 2>&1
	cat "$REGS" > "$dir/regs.txt" 2>&1
	cat "$AUTOLOG" > "$dir/auto_log.txt" 2>&1
	cat "$ROLE" > "$dir/role.txt" 2>&1
	for p in /sys/class/usb_role/lightning-role-switch/device/{accessory,accessory_gates,vbus_gates,revert_pending} \
		/sys/module/apple_tristar_power/parameters/{auto_host,auto_input_ua,allow_power_handshake} \
		/sys/module/apple_s8000_usb_role/parameters/{usb_ctl_phy_on,usb_ctl_force} \
		/sys/module/ehci_platform/parameters/apple_port_budget_ma; do
		printf '%s: %s\n' "$p" "$(cat "$p" 2>&1)"
	done > "$dir/knobs.txt"
	{ lsusb; echo; lsusb -t; } > "$dir/lsusb.txt" 2>&1
	lsblk -o NAME,TRAN,SIZE,FSTYPE,LABEL,RO,MOUNTPOINTS > "$dir/lsblk.txt" 2>&1
	for s in /sys/class/power_supply/*; do
		echo "== $s"; cat "$s/uevent"
	done > "$dir/power_supply.txt" 2>&1
	journalctl -k -b --no-pager -n 400 > "$dir/kernel.txt" 2>&1
	note "snapshot $(basename "$dir")"
}

# READ-ONLY mount check of a USB partition; never writes to it.
mount_check() {
	local dev=$1 log="$RUN/mount-$(basename "$dev").txt"
	{
		echo "device $dev"
		lsblk -o NAME,TRAN,SIZE,FSTYPE,LABEL,RO "$dev"
		if findmnt -n "$dev" >/dev/null; then
			echo "already mounted:"; findmnt -n -o TARGET,OPTIONS "$dev"
		elif mount -o ro,nosuid,nodev,noexec "$dev" "$MNT"; then
			echo "mounted read-only at $MNT:"; findmnt -n -o SOURCE,FSTYPE,OPTIONS "$MNT"
			df -h "$MNT"; ls -la "$MNT" | head -40
			umount "$MNT" && echo "unmounted"
		else
			echo "read-only mount FAILED"
		fi
	} > "$log" 2>&1
	note "mount check $dev: $(grep -m1 -E 'mounted read-only|already mounted|FAILED' "$log")"
}

note "usb-host5 logger start, boot $(cat /proc/sys/kernel/random/boot_id), kernel $(uname -r)"
note "cmdline entry: $(tr ' ' '\n' </proc/cmdline | grep linux_a9.entry)"
snapshot start
while [ ! -e "$OUT/stop" ] && [ "$(date +%s)" -lt "$end" ]; do
	if [ -r "$STATE" ] && cat "$STATE" > "$RUN/.state" 2>/dev/null; then
		phase=$(field phase "$RUN/.state")
		role=$(field role "$RUN/.state")
		id=$(field digital_id "$RUN/.state")
		ph=$(field power_handshake "$RUN/.state")
		online=$(grep -o 'online [01]' "$RUN/.state" | head -1)
		chg=$(sed -n 's/^charger: //p' "$RUN/.state" | grep -o 'input_current_limit_ua=[0-9-]*')
		bat=$(sed -n 's/^battery: //p' "$RUN/.state" | grep -o 'status=[A-Za-z ]*current' | sed 's/ current//')
		key="$phase|$role|$id|$ph|$online|$chg|$bat"
		if [ "$key" != "$prev_key" ]; then
			note "phase=$phase role=$role id=[$id] handshake=[$ph] supply=[$online] charger=[$chg] battery=[$bat]"
			snapshot "$phase"
			prev_key=$key
		fi
	else
		note "cannot read $STATE"
		sleep 5
	fi
	# USB disk partitions (TRAN usb on the parent disk): read-only check once
	# per attach; the record resets when no USB disk is left.
	usb_disks=0
	while read -r name tran; do
		[ "$tran" = usb ] || continue
		usb_disks=$((usb_disks + 1))
		for part in $(lsblk -nro NAME,TYPE "/dev/$name" | awk '$2=="part" {print $1}'); do
			key="$part-$(cat /sys/class/block/"$part"/size 2>/dev/null)"
			if [ -z "${mounted_seen[$key]:-}" ]; then
				mounted_seen[$key]=1
				mount_check "/dev/$part"
			fi
		done
	done < <(lsblk -dnro NAME,TRAN 2>/dev/null)
	[ "$usb_disks" -gt 0 ] || mounted_seen=()
	sleep 1
done
snapshot stop
note "usb-host5 logger end"
