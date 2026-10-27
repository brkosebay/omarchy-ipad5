#!/usr/bin/env bash
# Orchestrator only (Mac). Event-driven tooling for the usb-host5 camera-adapter
# test. The owner swaps cables whenever they like; nothing here counts down.
#
#   usb-host5-watch.sh install   copy the iPad-side logger (usb-host5-device.sh)
#   usb-host5-watch.sh start     start it on the iPad (systemd unit, no deadline)
#   usb-host5-watch.sh watch     follow the iPad timeline over Wi-Fi and the Mac's
#                                own view of the gadget; runs until Ctrl-C
#                                (WATCH_SECONDS>0 caps it). Survives Wi-Fi drops.
#   usb-host5-watch.sh status    one-shot lightning_state + role registers
#   usb-host5-watch.sh reenter [usb_ctl_phy_on=0|1] [accessory_gates=none|pmu]
#                                [port_budget_ma=N] [usb_ctl_force=0|3]
#                                new host entry with these knobs, adapter left
#                                plugged in (A/B ladder)
#   usb-host5-watch.sh usbctl 0x3   debug, host mode: write USB_CTL+0 now and
#                                show the readback, EHCI port and lsusb -t
#   usb-host5-watch.sh pull      copy the iPad logs to build/usb-host5/device-logs/
#   usb-host5-watch.sh stop      stop the iPad logger
#   usb-host5-watch.sh verdict [DIR]   pass/fail from pulled logs + watch log
#
# Control path: Wi-Fi SSH (OMARCHY_HOST, default build/omarchy/wifi-host), so
# the Mac cable can be out. The Mac-side gadget check uses the tether address.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH="${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}"
if [ -z "${OMARCHY_HOST:-}" ]; then
	[ -s "$ROOT/build/omarchy/wifi-host" ] || {
		echo 'run scripts/periph/usb-host-wifi.sh first (no build/omarchy/wifi-host)' >&2; exit 2; }
	OMARCHY_HOST=$(cat "$ROOT/build/omarchy/wifi-host")
fi
export OMARCHY_HOST
TETHER=${TETHER:-172.16.42.1}
REMOTE_LOG=/var/log/linux-a9-usb-host5
LOGGER=/usr/local/sbin/linux-a9-usb-host5-log
UNIT=linux-a9-usb-host5-log
OUTDIR="$ROOT/build/usb-host5"
mkdir -p "$OUTDIR"

ipad() { "$SSH" "$@"; }

tether_state() {
	local cu=no ping=no
	ls /dev/cu.usbmodem* >/dev/null 2>&1 && cu=yes
	ping -c1 -t1 "$TETHER" >/dev/null 2>&1 && ping=yes
	echo "acm=$cu ecm_ping=$ping"
}

case "${1:-}" in
install)
	ipad 'cat > /tmp/linux-a9-usb-host5-log.sh' < "$ROOT/scripts/periph/usb-host5-device.sh" &&
		ipad "sudo -n install -m 0755 /tmp/linux-a9-usb-host5-log.sh $LOGGER && rm -f /tmp/linux-a9-usb-host5-log.sh && sha256sum $LOGGER"
	shasum -a 256 "$ROOT/scripts/periph/usb-host5-device.sh"
	;;
start)
	ipad "sudo -n systemctl stop $UNIT 2>/dev/null; sudo -n systemctl reset-failed $UNIT 2>/dev/null; sudo -n systemd-run --unit=$UNIT --collect $LOGGER && sleep 2 && systemctl is-active $UNIT && sudo -n tail -3 $REMOTE_LOG/latest/timeline.txt"
	;;
stop)
	ipad "sudo -n touch $REMOTE_LOG/stop; sleep 3; sudo -n systemctl stop $UNIT 2>/dev/null; systemctl is-active $UNIT || true"
	;;
status)
	ipad "cat /sys/bus/i2c/devices/0-001a/lightning_state; echo; sudo -n cat /sys/kernel/debug/apple-usb-role/regs; echo; sudo -n tail -20 $REMOTE_LOG/latest/timeline.txt"
	echo "mac: $(tether_state)"
	;;
watch)
	log="$OUTDIR/watch-$(date -u +%Y%m%dT%H%M%SZ).log"
	ln -sfn "$(basename "$log")" "$OUTDIR/watch-latest.log"
	start=$(date +%s)
	seen=0
	ssh_ok=unknown
	mac_prev=""
	adapter_gone=no
	echo "watching (Ctrl-C to end); log: $log" | tee -a "$log"
	while :; do
		if [ "${WATCH_SECONDS:-0}" -gt 0 ] && [ $(( $(date +%s) - start )) -ge "$WATCH_SECONDS" ]; then
			echo "$(date -u +%T) WATCH_SECONDS reached" | tee -a "$log"; break
		fi
		out=$(ipad "readlink -f $REMOTE_LOG/latest && tail -n +$((seen + 1)) $REMOTE_LOG/latest/timeline.txt" 2>/dev/null)
		rc=$?
		if [ $rc -eq 0 ]; then
			[ "$ssh_ok" = yes ] || echo "$(date -u +%T) MAC: Wi-Fi SSH ok" | tee -a "$log"
			ssh_ok=yes
			run=$(printf '%s\n' "$out" | head -1)
			lines=$(printf '%s\n' "$out" | tail -n +2)
			if [ "$run" != "${run_prev:-}" ]; then
				# A restarted logger is a new run: follow it from its start.
				[ -z "${run_prev:-}" ] || {
					echo "$(date -u +%T) MAC: new logger run $run" | tee -a "$log"
					out=$(ipad "cat $run/timeline.txt" 2>/dev/null); lines=$out; seen=0; }
				run_prev=$run
			fi
			if [ -n "$lines" ]; then
				printf '%s\n' "$lines" | sed 's/^/IPAD /' | tee -a "$log"
				seen=$((seen + $(printf '%s\n' "$lines" | wc -l)))
				if printf '%s\n' "$lines" | grep -q 'phase=empty'; then
					adapter_gone=yes
				fi
			fi
		elif [ "$ssh_ok" != no ]; then
			echo "$(date -u +%T) MAC: Wi-Fi SSH unreachable (rc $rc); retrying" | tee -a "$log"
			ssh_ok=no
		fi
		mac=$(tether_state)
		if [ "$mac" != "$mac_prev" ]; then
			echo "$(date -u +%T) MAC: gadget $mac" | tee -a "$log"
			mac_prev=$mac
			# Gadget back on the Mac: SSH over the tether answers this boot.
			if [ "$adapter_gone" = yes ] && [ "${mac#*ecm_ping=}" = yes ]; then
				boot=$(OMARCHY_HOST=$TETHER "$SSH" 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null)
				wboot=$(ipad 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null)
				if [ -n "$boot" ] && [ "$boot" = "$wboot" ]; then
					echo "$(date -u +%T) MAC: GADGET OK - tether SSH answers, same boot $boot" | tee -a "$log"
				else
					echo "$(date -u +%T) MAC: tether ping ok, SSH boot [$boot] vs Wi-Fi [$wboot]" | tee -a "$log"
				fi
			fi
		fi
		sleep 2
	done
	;;
reenter)
	# A/B without touching the cable: policy off, device role, knobs, policy
	# on. The policy then identifies the attached adapter again and makes a
	# fresh host entry with the new knobs. Knobs: usb_ctl_phy_on=0|1,
	# accessory_gates=none|pmu, port_budget_ma=N.
	shift
	cmds="set -e; P=/sys/module; R=/sys/class/usb_role/lightning-role-switch"
	cmds+="; echo 0 | sudo -n tee \$P/apple_tristar_power/parameters/auto_host >/dev/null"
	cmds+="; echo device | sudo -n tee \$R/role >/dev/null"
	for kv in "$@"; do
		k=${kv%%=*}; v=${kv#*=}
		[[ "$v" =~ ^[a-z0-9]+$ ]] || { echo "bad value: $kv" >&2; exit 2; }
		case "$k" in
		usb_ctl_phy_on) cmds+="; echo $v | sudo -n tee \$P/apple_s8000_usb_role/parameters/usb_ctl_phy_on >/dev/null" ;;
		accessory_gates) cmds+="; echo $v | sudo -n tee \$R/device/accessory_gates >/dev/null" ;;
		port_budget_ma) cmds+="; for m in ehci_platform ohci_platform; do echo $v | sudo -n tee \$P/\$m/parameters/apple_port_budget_ma >/dev/null; done" ;;
		usb_ctl_force) cmds+="; echo $v | sudo -n tee \$P/apple_s8000_usb_role/parameters/usb_ctl_force >/dev/null" ;;
		*) echo "unknown knob: $k" >&2; exit 2 ;;
		esac
	done
	cmds+="; sleep 1; echo 1 | sudo -n tee \$P/apple_tristar_power/parameters/auto_host >/dev/null"
	cmds+="; echo 'usb-host5: reenter $*' | sudo -n tee /dev/kmsg >/dev/null; echo reenter requested: $*"
	ipad "$cmds"
	;;
usbctl)
	# Debug, host mode only: write USB_CTL+0 now (bit 0 kept), log readbacks.
	v=${2:-}
	[[ "$v" =~ ^(0x)?[0-3]$ ]] || { echo 'usage: usbctl 0x3|0x1' >&2; exit 2; }
	ipad "echo 'usb-host5: usbctl $v' | sudo -n tee /dev/kmsg >/dev/null; echo $v | sudo -n tee /sys/kernel/debug/apple-usb-role/usb_ctl >/dev/null; sudo -n cat /sys/kernel/debug/apple-usb-role/usb_ctl; sudo -n grep -E '^usb_ctl|^ehci:' /sys/kernel/debug/apple-usb-role/regs; journalctl -k -b --no-pager | grep -E 'USB_CTL|usb [0-9]|hub' | tail -8; lsusb -t"
	;;
pull)
	run=$(ipad "readlink -f $REMOTE_LOG/latest") || exit 1
	dst="$OUTDIR/device-logs/$(basename "$run")"
	mkdir -p "$dst"
	ipad "sudo -n tar -C '$run' -cf - ." | tar -C "$dst" -xf - && echo "$dst"
	;;
verdict)
	dir=${2:-$(ls -d "$OUTDIR"/device-logs/*/ 2>/dev/null | tail -1)}
	[ -n "$dir" ] || { echo 'no pulled logs; run pull first' >&2; exit 2; }
	python3 -B -I "$ROOT/scripts/periph/usb-host5-verdict.py" "$dir" \
		--watch-log "$OUTDIR/watch-latest.log"
	;;
*)
	sed -n '2,23p' "$0" >&2
	exit 2
	;;
esac
