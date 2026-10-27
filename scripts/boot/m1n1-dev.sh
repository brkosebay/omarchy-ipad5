#!/usr/bin/env bash
# Developer path (docs/boot/warm-reboot.md, "m1n1 handoff"): kernel iteration
# from the Mac without DFU. Linux kexecs back into the m1n1 that booted it
# (it stays resident; needs the PSCI m1n1 and psci=1), m1n1 runs its USB
# proxy, and the Mac boots a new kernel through it with m1n1's own DT prep.
#
#   scripts/boot/m1n1-dev.sh enter            iPad: Linux -> m1n1 proxy (over SSH)
#   [TREE=integ] scripts/boot/m1n1-dev.sh linux [extra bootargs...]
#                                             Mac: boot build/kernel-$TREE through the proxy
#   scripts/boot/m1n1-dev.sh shell            m1n1 proxy shell (poke hardware)
#
# The proxy client comes from build/m1n1-boot/proxyclient (build-m1n1.sh) and
# needs pyserial + construct (a venv in build/boot/venv is made on first use).
# A *new* m1n1 can't be loaded this way (it would not own EL3, so no PSCI):
# m1n1 changes still need a cold boot.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH="${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}"
PC="$ROOT/build/m1n1-boot/proxyclient"
VENV="$ROOT/build/boot/venv"
TREE="${TREE:-bootinteg}"
K="$ROOT/build/kernel-$TREE"

m1n1_tty() { # the newest m1n1 ACM port (first of its two interfaces)
	local t=${1:-60}
	while [ "$t" -gt 0 ]; do
		if [ "$("$ROOT/scripts/boot/autoboot.sh" --state)" = m1n1 ]; then
			ls -t /dev/cu.usbmodem* 2>/dev/null | sort | head -1
			return 0
		fi
		sleep 1
		t=$((t - 1))
	done
	echo "no m1n1 proxy on USB" >&2
	return 1
}

py() {
	[ -d "$PC" ] || { echo "no $PC: run scripts/boot/build-m1n1.sh" >&2; exit 1; }
	if [ ! -x "$VENV/bin/python3" ]; then
		python3 -m venv "$VENV"
		"$VENV/bin/pip" -q install pyserial construct
	fi
	"$VENV/bin/python3" "$@"
}

case "${1:-}" in
enter)
	"$SSH" 'sudo /usr/local/bin/linux-a9-kexec m1n1' || true # the link drops mid-command
	m1n1_tty 60 >/dev/null && echo "m1n1 proxy is up"
	;;
linux)
	shift
	tty=$(m1n1_tty 10)
	dtb="$K/dtbs/${DTB:-s8003-j71t.dtb}"
	initrd="${INITRD:-$ROOT/build/boot/initramfs-omarchy.gz}"
	ww=$(grep '^WRITE_WINDOW=' "$ROOT/build/nvme-install/layout.env" | cut -d= -f2)
	args="earlycon console=tty0 loglevel=7 hid_apple.fnmode=2 nvme.write_window=$ww omarchy.root=nvme panic=0 apple_wdt.cold_restart=0 crashkernel=${CRASHKERNEL:-256M} $*"
	echo "booting $K ($(cut -c1-12 "$K/COMMIT")) via $tty: $args"
	M1N1DEVICE="$tty" py "$PC/tools/linux.py" -b "$args" "$K/Image.gz" "$dtb" "$initrd"
	;;
shell)
	M1N1DEVICE="$(m1n1_tty 10)" py "$PC/tools/shell.py"
	;;
*) sed -n '2,17p' "$0"; exit 2 ;;
esac
