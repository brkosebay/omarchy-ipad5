#!/usr/bin/env bash
# Serve build/omarchy/rootfs.img to the iPad over NBD: native qemu-nbd
# (Homebrew qemu) on the Mac's USB-gadget address 172.16.42.2, port 10809,
# export "omarchy". The NBD-root initramfs (scripts/omarchy/initramfs/init)
# waits for it, so this can start before or after the iPad boots.
#
# Usage: scripts/omarchy/serve-root.sh [--bind ADDR] [--wait SECONDS] [--snapshot] [IMAGE]
#   --bind ADDR    172.16.42.2 (default) or 127.0.0.1 (offline tests only)
#   --wait SECS    how long to wait for ADDR to appear on an interface [900]
#   --snapshot     writes go to a throwaway temporary file, the image stays as
#                  it is (a test boot that must not use up the first boot)
#   IMAGE          default build/omarchy/rootfs.img
# Stop with Ctrl-C, and only once the iPad has shut down or is in the rescue
# shell: the iPad's root filesystem is this file.
#
# The export is unauthenticated and read-write, so it is never bound to
# 0.0.0.0: only the point-to-point USB link (or loopback) can reach it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIND=172.16.42.2
WAIT=900
IMG="$ROOT/build/omarchy/rootfs.img"
SNAPSHOT=()
while [ $# -gt 0 ]; do
	case "$1" in
	--bind) BIND="$2"; shift 2 ;;
	--wait) WAIT="$2"; shift 2 ;;
	--snapshot) SNAPSHOT=(--snapshot); shift ;;
	-*) echo "unknown option: $1" >&2; exit 1 ;;
	*) IMG="$1"; shift ;;
	esac
done

case "$BIND" in
172.16.42.2 | 127.0.0.1) ;;
*) echo "refusing to bind $BIND: the export is an unauthenticated read-write root" >&2; exit 1 ;;
esac
command -v qemu-nbd >/dev/null || { echo "qemu-nbd missing: brew install qemu" >&2; exit 1; }
[ -f "$IMG" ] || { echo "missing $IMG (scripts/omarchy/build-rootfs.sh)" >&2; exit 1; }

# Two writers on one ext4 image corrupt it: not while the build has it
# loop-mounted, and not twice.
if docker ps --format '{{.Image}}' 2>/dev/null | grep -q '^linux-a9-alarm'; then
	echo "a linux-a9-alarm build container is running (it may have the image mounted); stop it first" >&2
	exit 1
fi
if pgrep -f "qemu-nbd.*$(basename "$IMG")" >/dev/null; then
	echo "another qemu-nbd already serves $(basename "$IMG")" >&2
	exit 1
fi

# writeback: the Mac's page cache absorbs writes and an NBD flush from the
# iPad (ext4 commit/barrier) becomes an fsync of the image. discard and
# detect-zeroes=unmap keep the sparse image sparse when the iPad trims.
# One client at a time (--shared=1): two kernels on one ext4 corrupt it.
# Always run as `serve &`: exec makes the background job qemu-nbd itself, so
# killing $pid stops the server rather than a subshell that leaves it running.
serve() {
	exec qemu-nbd \
		--format=raw \
		--export-name=omarchy \
		--description="linux-a9 Omarchy rootfs" \
		--bind="$BIND" --port=10809 \
		--persistent --shared=1 \
		--cache=writeback --aio=threads \
		--discard=unmap --detect-zeroes=unmap \
		${SNAPSHOT[@]+"${SNAPSHOT[@]}"} \
		"$IMG"
}

if [ "$BIND" = 127.0.0.1 ]; then
	echo "serving $IMG as export 'omarchy' on 127.0.0.1:10809 (Ctrl-C to stop)"
	# In the background with a trap, so a TERM to this script (qemu-smoke.sh's
	# cleanup) stops qemu-nbd too instead of orphaning it.
	serve &
	pid=$!
	trap 'kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; exit 0' INT TERM
	wait "$pid"
	exit
fi

has_ip() { ifconfig | grep -q 'inet 172\.16\.42\.2 '; }

pid=""
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; wait 2>/dev/null; echo; echo "stopped"; exit 0' INT TERM

# An iPad that resets sends no FIN, so its old connection would hold the one
# client slot and refuse the next boot. Each time the USB link goes away for
# 20 s (every reboot does), the server restarts; shorter USB glitches keep
# the session.
while :; do
	echo "waiting up to ${WAIT}s for 172.16.42.2 on a USB interface (boot the iPad)..."
	t=0
	until has_ip; do
		[ "$t" -ge "$WAIT" ] && { echo "172.16.42.2 never appeared; is the iPad's USB network up?" >&2; exit 1; }
		sleep 2
		t=$((t + 2))
	done
	iface="$(ifconfig | awk '/^[a-z]/{i=$1} /inet 172\.16\.42\.2 /{sub(":","",i); print i}')"
	echo "$(date +%T) 172.16.42.2 is up on $iface; serving $IMG as export 'omarchy' on 172.16.42.2:10809"
	serve &
	pid=$!
	gone=0
	while kill -0 "$pid" 2>/dev/null; do
		sleep 2
		if has_ip; then gone=0; else gone=$((gone + 2)); fi
		if [ "$gone" -ge 20 ]; then
			echo "$(date +%T) USB link gone for 20 s (iPad rebooting?); restarting the server"
			kill "$pid" 2>/dev/null
			wait "$pid" 2>/dev/null || true
			break
		fi
	done
	pid=""
	sleep 1
done
