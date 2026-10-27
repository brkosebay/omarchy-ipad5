#!/usr/bin/env bash
# Offline tests of the nvme-install pieces under QEMU + HVF on the Mac, with
# QEMU's emulated NVMe controller (4 KiB-LBA namespaces) standing in for the
# iPad's. The Apple-specific code (ANS glue, FlatDMA, the system-namespace
# guard) isn't reached, because QEMU's controller isn't Apple's. Patch 0001's
# generic gates and the initramfs root selector are.
#
#   scripts/nvme-install/qemu-test.sh kernel   # build build/nvme-install/qemu/Image
#   scripts/nvme-install/qemu-test.sh fence    # nvme.write_window: allowed and refused writes, checked on the host
#   scripts/nvme-install/qemu-test.sh init     # omarchy.root=nvme|nbd selector and its fallbacks
#   scripts/nvme-install/qemu-test.sh tool     # nvme-install.py (the on-device tool) end to end
#
# Kernel: committed a9/nvme-install + build/kernel-nvme-install/config +
# scripts/omarchy/qemu/qemu-virt.fragment, built in its own Docker volume
# (linux-a9-nvi-qemu), so no shared tree is touched. Results:
# build/nvme-install/qemu/{fence,init}-report.txt.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
Q="$ROOT/build/nvme-install/qemu"
T="$ROOT/scripts/nvme-install/qemu"
mkdir -p "$Q"
MODE="${1:?kernel|fence|init|tool}"

build_kernel() {
	[ -f "$ROOT/build/kernel-nvme-install/config" ] || "$ROOT/scripts/nvme-install/build-kernel.sh"
	docker run --rm --platform linux/arm64 -v linux-a9-src:/src:ro -v linux-a9-nvi-qemu:/q \
		-v "$ROOT:/work" linux-a9-kbuild bash -euc '
		git config --global --add safe.directory "*"
		rev=$(git -C /src/linux rev-parse a9/nvme-install)
		if [ "$(cat /q/REV 2>/dev/null)" != "$rev" ]; then
			rm -rf /q/src && mkdir -p /q/src
			git -C /src/linux archive a9/nvme-install | tar -x -C /q/src
			echo "$rev" >/q/REV
		fi
		cd /q/src
		cp /work/build/kernel-nvme-install/config .config
		scripts/kconfig/merge_config.sh -m .config /work/scripts/omarchy/qemu/qemu-virt.fragment >/dev/null
		make -s LLVM=1 ARCH=arm64 olddefconfig
		make -s -j"$(nproc)" LLVM=1 ARCH=arm64 Image
		cp arch/arm64/boot/Image /work/build/nvme-install/qemu/Image
		cp .config /work/build/nvme-install/qemu/config
		echo "$rev" >/work/build/nvme-install/qemu/COMMIT
		grep -E "^CONFIG_(NVME_PCI_WRITE_WINDOW|NVME_PCI_READ_ONLY|BLK_DEV_NVME|PCI_HOST_GENERIC|ARM64_16K_PAGES)=y" .config'
}

# qemu_run NAME TIMEOUT APPEND INITRD [qemu args...]: serial console -> $Q/NAME.log
qemu_run() {
	local name=$1 secs=$2 append=$3 initrd=$4
	shift 4
	local pid t=0
	rm -f "$Q/$name.log"
	qemu-system-aarch64 \
		-machine virt -accel hvf -cpu host -smp 2 -m 1024 \
		-kernel "$Q/Image" -initrd "$initrd" \
		-append "console=ttyAMA0 loglevel=6 panic=-1 $append" \
		-display none -serial file:"$Q/$name.log" -monitor none -no-reboot \
		"$@" &
	pid=$!
	# The guest powers itself off; a hung or held boot is killed (macOS has no timeout(1))
	while kill -0 "$pid" 2>/dev/null && [ "$t" -lt "$secs" ]; do
		sleep 1
		t=$((t + 1))
	done
	kill "$pid" 2>/dev/null && echo "$name: killed after ${secs}s" || true
	wait "$pid" 2>/dev/null || true
}

nvme_args() { # nvme_args NS1 [NS2]: one controller, 4 KiB-LBA namespaces
	local a=(-device nvme,id=nvme0,serial=nvi0001
		-drive "file=$1,if=none,id=ns1,format=raw"
		-device nvme-ns,drive=ns1,bus=nvme0,nsid=1,logical_block_size=4096,physical_block_size=4096)
	if [ -n "${2:-}" ]; then
		a+=(-drive "file=$2,if=none,id=ns2,format=raw"
			-device nvme-ns,drive=ns2,bus=nvme0,nsid=2,logical_block_size=4096,physical_block_size=4096)
	fi
	printf '%s\n' "${a[@]}"
}

fence() {
	[ -f "$Q/Image" ] || build_kernel
	# Test initramfs: static BusyBox + wtest + fence-init.sh as /init.
	docker run --rm --platform linux/arm64 -v "$T:/t:ro" -v "$Q:/q" alpine:edge sh -euc '
		apk add --no-cache busybox-static gcc musl-dev linux-headers cpio >/dev/null
		r=/tmp/r; mkdir -p $r/bin $r/proc $r/sys $r/dev
		cp /bin/busybox.static $r/bin/busybox
		gcc -static -O2 -Wall -o $r/bin/wtest /t/wtest.c
		install -m 0755 /t/fence-init.sh $r/init
		cd $r && find . | cpio -o -H newc -R 0:0 --quiet | gzip -9n >/q/fence-initramfs.gz'
	python3 -I -c '
import os, sys
for name, lbas in (("ns1", 16384), ("ns2", 1024)):
    with open(sys.argv[1] + "/fence-" + name + ".img", "wb") as f:
        f.write(os.urandom(lbas * 4096))' "$Q"
	local rep="$Q/fence-report.txt" rc=0 c ns1 ns2 nvme=()
	: >"$rep"
	ns1="$Q/fence-ns1.img" ns2="$Q/fence-ns2.img"
	mapfile -t nvme < <(nvme_args "$ns1" "$ns2")
	for c in window none bad ns2; do
		case $c in
		window) arg="nvme.write_window=1:4096-12287" ;;
		none) arg="" ;;
		bad) arg="nvme.write_window=1:12287-4096" ;;
		ns2) arg="nvme.write_window=2:0-1023" ;;
		esac
		cp "$ns1" "$Q/before-ns1.img"
		cp "$ns2" "$Q/before-ns2.img"
		qemu_run "fence-$c" 120 "$arg" "$Q/fence-initramfs.gz" "${nvme[@]}"
		{ echo "## $c: $arg"
		  python3 -I "$T/check-fence.py" "$c" "$Q/fence-$c.log" "$Q/before-ns1.img" "$ns1" "$Q/before-ns2.img" "$ns2" || rc=1
		} >>"$rep"
	done
	rm -f "$Q/before-ns1.img" "$Q/before-ns2.img"
	cat "$rep"
	echo "fence: $(grep -c '^PASS' "$rep") PASS, $(grep -c '^FAIL' "$rep") FAIL (report $rep)"
	return $rc
}

init() {
	[ -f "$Q/Image" ] || build_kernel
	local base="$ROOT/build/omarchy/initramfs-nbd.gz"
	[ -f "$base" ] || { echo "missing $base (scripts/omarchy/build-initramfs.sh)" >&2; exit 1; }
	"$ROOT/scripts/nvme-install/build-initramfs.sh"
	local initrd="$ROOT/build/nvme-install/initramfs-omarchy.gz"
	# Two tiny roots whose "systemd" reports what the initramfs mounted, then
	# powers off; the NVMe disk is GPT on 4 KiB LBAs, p2 = the NVMe root.
	docker run --rm --privileged --platform linux/arm64 -v "$Q:/q" alpine:edge sh -euc '
		apk add --no-cache busybox-static e2fsprogs sgdisk util-linux >/dev/null
		mkroot() {
			r=/tmp/root-$1; rm -rf $r; mkdir -p $r/bin $r/proc $r/sys $r/dev $r/run $r/usr/lib/systemd
			cp /bin/busybox.static $r/bin/busybox
			printf "%s\n" "#!/bin/busybox sh" \
				"echo \"TESTROOT $1 root=\$(/bin/busybox awk '\''\$2==\"/\"{d=\$1} END{print d}'\'' /proc/mounts)\"" \
				"/bin/busybox poweroff -f" >$r/usr/lib/systemd/systemd
			chmod 0755 $r/usr/lib/systemd/systemd
			rm -f $2; mke2fs -q -t ext4 -b 4096 -L $3 -d $r $2 $4
		}
		mkroot nvme /q/init-p2.ext4 omarchy-nvme 24M
		mkroot nbd /q/init-nbd.ext4 omarchy-root 24M
		# 96 MiB disk: p1 (stand-in for APFS) 6-12287, p2 16384-22527 (24 MiB).
		# Built in the container: a loop device on the bind-mounted Mac file
		# silently lost sgdisk'"'"'s writes.
		d=/tmp/ns1.img; truncate -s 96M $d
		l=$(losetup -b 4096 -f --show $d)
		sgdisk -a 2 -n 1:6:12287 -t 1:AF0A -c 1:Container -n 2:16384:22527 -t 2:8300 -c 2:omarchy-root "$l" >/dev/null
		sgdisk -p "$l" | tail -2
		losetup -d "$l"
		dd if=/q/init-p2.ext4 of=$d bs=4096 seek=16384 conv=notrunc status=none
		cp $d /q/init-ns1.img'
	qemu-nbd --format=raw --export-name=omarchy --bind=127.0.0.1 --port=10811 --persistent --shared=1 \
		--snapshot "$Q/init-nbd.ext4" >"$Q/init-qemu-nbd.log" 2>&1 &
	local nbd_pid=$! rep="$Q/init-report.txt" rc=0 nvme=() name want secs args
	trap 'kill '"$nbd_pid"' 2>/dev/null || true' EXIT
	sleep 1
	mapfile -t nvme < <(nvme_args "$Q/init-ns1.img")
	local net=(-netdev user,net=192.0.2.0/24,host=192.0.2.2,dhcpstart=192.0.2.15,id=n0 -device virtio-net-pci,netdev=n0)
	local common="loglevel=7 omarchy.ip=eth0:192.0.2.15/24:192.0.2.2 omarchy.nbd=192.0.2.2:10811/omarchy omarchy.wait=60 omarchy.nvme_wait=20 omarchy.netconsole_wait=2"
	local win="nvme.write_window=1:16384-22527"
	: >"$rep"
	# name | expected marker | timeout | kernel args | with NVMe?
	while IFS='|' read -r name want secs args disk; do
		[ -n "$name" ] || continue
		if [ "$disk" = yes ]; then
			qemu_run "init-$name" "$secs" "$common $args" "$initrd" "${nvme[@]}" "${net[@]}"
		else
			qemu_run "init-$name" "$secs" "$common $args" "$initrd" "${net[@]}"
		fi
		if grep -q -- "$want" "$Q/init-$name.log"; then
			echo "PASS $name: found '$want'" >>"$rep"
		else
			echo "FAIL $name: no '$want'; tail:" >>"$rep"; tail -15 "$Q/init-$name.log" | sed 's/^/    /' >>"$rep"; rc=1
		fi
	done <<EOF
nvme-root|TESTROOT nvme root=/dev/nvme0n1p2|120|omarchy.root=nvme $win|yes
explicit-dev|TESTROOT nvme root=/dev/nvme0n1p2|120|omarchy.root=nvme omarchy.nvme=/dev/nvme0n1p2 $win|yes
wrong-dev|TESTROOT nbd root=/dev/nbd0|120|omarchy.root=nvme omarchy.nvme=/dev/nvme0n1p1 $win|yes
default-nbd|TESTROOT nbd root=/dev/nbd0|120||yes
nbd-explicit|TESTROOT nbd root=/dev/nbd0|120|omarchy.root=nbd $win|yes
no-window|TESTROOT nbd root=/dev/nbd0|120|omarchy.root=nvme|yes
stale-window|TESTROOT nbd root=/dev/nbd0|120|omarchy.root=nvme nvme.write_window=1:16384-22526|yes
wrong-label|TESTROOT nbd root=/dev/nbd0|120|omarchy.root=nvme omarchy.label=other $win|yes
no-nvme|TESTROOT nbd root=/dev/nbd0|150|omarchy.root=nvme $win|no
rescue|RESCUE: no usable NVMe root|60|omarchy.root=nvme omarchy.fallback=rescue|yes
EOF
	# The reason for each fallback, from the initramfs log on the console.
	for pair in "no-window|no nvme.write_window on the kernel command line" \
		"stale-window|No NVMe partition is exactly the write window 1:16384-22526" \
		"wrong-label|labelled 'omarchy-nvme', expected 'other'" \
		"wrong-dev|/dev/nvme0n1p1 is not the write window" \
		"no-nvme|No NVMe root (window) after 20s" "nvme-root|/dev/nvme0n1p2 is the write window 1:16384-22527"; do
		name=${pair%%|*} want=${pair#*|}
		if grep -qF -- "$want" "$Q/init-$name.log"; then echo "PASS $name: reason '$want'" >>"$rep"
		else echo "FAIL $name: reason '$want' not logged" >>"$rep"; rc=1; fi
	done
	kill "$nbd_pid" 2>/dev/null || true
	cat "$rep"
	echo "init: $(grep -c '^PASS' "$rep") PASS, $(grep -c '^FAIL' "$rep") FAIL (report $rep)"
	return $rc
}

# The on-device tool (nvme-install.py) against QEMU's NVMe holding the GPT
# that test-gpt.sh planned: a read-only boot (writes must be refused), then a
# boot with the write window (checks, fence test, soak, image write/verify,
# nothing outside p2 changed). Root is a small Alpine with python3 + blkid.
tool() {
	[ -f "$Q/Image" ] || build_kernel
	local TG="$ROOT/build/nvme-install/test-gpt" rc=0 rep="$Q/tool-report.txt"
	[ -f "$TG/layout.env" ] || "$ROOT/scripts/nvme-install/test-gpt.sh" >/dev/null
	docker run --rm --platform linux/arm64 -v "$ROOT/scripts/nvme-install:/s:ro" -v "$TG:/tg:ro" -v "$Q:/q" \
		alpine:edge sh -euc '
		apk add --no-cache e2fsprogs >/dev/null
		r=/tmp/r; mkdir -p $r
		apk --root $r --initdb --keys-dir /etc/apk/keys --repositories-file /etc/apk/repositories \
			add --no-cache busybox python3 blkid zstd coreutils >/dev/null
		mkdir -p $r/nvi $r/proc $r/sys $r/dev $r/tmp
		cp /s/nvme-install.py /tg/layout.env $r/nvi/
		install -m 0755 /s/qemu/tool-init.sh $r/nvi/test-init.sh
		rm -f /q/tool-root.ext4; mke2fs -q -t ext4 -L toolroot -d $r /q/tool-root.ext4 1G'
	# 32,000,000,000-byte namespace with the planned GPT (sparse); NSID 2 small
	python3 -I - "$TG" "$Q/tool-ns1.img" "$Q/tool-ns2.img" <<'EOF'
import os, sys
tg, ns1, ns2 = sys.argv[1:]
first = open(tg + "/backup/nvme0n1-first-1MiB.bin", "rb").read()
new_p = open(tg + "/gpt-plan/new-primary.bin", "rb").read()
new_b = open(tg + "/gpt-plan/new-backup.bin", "rb").read()
size = 32000000000
for p in (ns1,):
    if os.path.exists(p): os.unlink(p)
fd = os.open(ns1, os.O_RDWR | os.O_CREAT, 0o644)
os.ftruncate(fd, size)
os.pwrite(fd, first[:4096], 0)
os.pwrite(fd, new_p, 4096)
os.pwrite(fd, new_b, size - len(new_b))
os.pwrite(fd, os.urandom(1 << 20), 3145734 * 4096)   # gap A content
os.pwrite(fd, os.urandom(1 << 20), 7812096 * 4096)   # gap B content
os.close(fd)
open(ns2, "wb").write(os.urandom(8 << 20))
EOF
	local win
	win=$(grep '^WRITE_WINDOW=' "$TG/layout.env" | cut -d= -f2)
	local args=(-device nvme,id=nvme0,serial=nvi0002
		-drive "file=$Q/tool-ns1.img,if=none,id=ns1,format=raw"
		-device nvme-ns,drive=ns1,bus=nvme0,nsid=1,logical_block_size=4096,physical_block_size=4096
		-drive "file=$Q/tool-ns2.img,if=none,id=ns2,format=raw"
		-device nvme-ns,drive=ns2,bus=nvme0,nsid=2,logical_block_size=4096,physical_block_size=4096
		-drive "file=$Q/tool-root.ext4,if=none,id=root,format=raw" -device virtio-blk-pci,drive=root)
	# qemu_run needs an initrd; the root is the virtio disk
	echo | gzip >"$Q/empty.gz"
	qemu_run tool-ro 300 "root=/dev/vda rw init=/nvi/test-init.sh" "$Q/empty.gz" "${args[@]}"
	qemu_run tool-window 900 "root=/dev/vda rw init=/nvi/test-init.sh nvme.write_window=$win" "$Q/empty.gz" "${args[@]}"
	: >"$rep"
	chk() { # chk LOG PATTERN WHAT
		if grep -qE -- "$2" "$Q/$1.log"; then echo "PASS $3" >>"$rep"; else echo "FAIL $3 (no /$2/ in $1.log)" >>"$rep"; rc=1; fi
	}
	chk tool-ro 'TOOL-TEST done' "read-only boot ran to the end"
	chk tool-ro '=== rc=0 \(check\)' "read-only kernel: guards pass (--no-window)"
	chk tool-ro 'REFUSED guards failed' "read-only kernel: soak refuses"
	chk tool-ro '=== rc=2 \(soak\)' "read-only kernel: soak exits 2"
	chk tool-window 'TOOL-TEST done' "window boot ran to the end"
	for c in info check hash-outside fence-test soak verify write-image verify-image; do
		chk tool-window "=== rc=0 \\($c\\)" "window: $c exits 0"
	done
	chk tool-window 'PASS write gap A first LBA .*(I/O error|Input/output error)' "fence: gap A write refused with EIO"
	chk tool-window "PASS write 1 MiB across p2's end .*(I/O error|Input/output error)" "fence: write across p2's end refused"
	chk tool-window 'PASS gap-a unchanged after the refused writes' "fence: gap A unchanged"
	chk tool-window 'PASS gap-b unchanged after the refused writes' "fence: gap B unchanged"
	chk tool-window 'PASS BLKDISCARD on p2: (Not supported|Operation not supported)' "trim refused before the device"
	chk tool-window 'PASS NSID 2 \(nvme0n.\) write: Operation not permitted' "NSID 2 write refused (EPERM)"
	chk tool-window 'PASS page-cache writes after drop_caches' "page-cache path verified"
	chk tool-window 'PASS p2 holds exactly the Mac.s image' "image written and read back"
	chk tool-window 'PASS REGION p1 .* unchanged' "p1 unchanged after everything"
	chk tool-window 'PASS REGION gpt-backup .* unchanged' "backup GPT unchanged"
	chk tool-window '=== rc=2 \(stale-layout\)' "a layout that doesn't match the disk is refused"
	# FAIL lines are expected only from the deliberately stale layout at the end
	if sed '/=== soak with a layout whose p2/,$d' "$Q/tool-window.log" | grep -qE '^FAIL'; then
		echo "FAIL the tool reported FAIL lines:" >>"$rep"
		sed '/=== soak with a layout whose p2/,$d' "$Q/tool-window.log" | grep -E '^FAIL' | sed 's/^/    /' >>"$rep"; rc=1
	fi
	grep -E '^INFO' "$Q/tool-window.log" | sed 's/^/    /' >>"$rep"
	rm -f "$Q/tool-ns1.img" "$Q/tool-ns2.img" "$Q/tool-root.ext4"
	cat "$rep"
	echo "tool: $(grep -c '^PASS' "$rep") PASS, $(grep -c '^FAIL' "$rep") FAIL (report $rep)"
	return $rc
}

case "$MODE" in
kernel) build_kernel ;;
fence) fence ;;
init) init ;;
tool) tool ;;
*) echo "unknown mode: $MODE" >&2; exit 1 ;;
esac
