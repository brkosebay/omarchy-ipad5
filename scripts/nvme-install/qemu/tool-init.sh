#!/bin/sh
# PID 1 of the QEMU device-tool test (scripts/nvme-install/qemu-test.sh tool):
# runs scripts/nvme-install/nvme-install.py the way the test plan does on the
# iPad, against QEMU's NVMe with the planned GPT. The host checks the
# "=== rc=" and PASS/FAIL/REFUSED lines on the console.
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
mount -t tmpfs tmp /tmp
T=/nvi
L=$T/layout.env
run() {
	echo "=== $*"
	python3 $T/nvme-install.py "$@" --layout $L
	echo "=== rc=$? ($1)"
}
i=0
while [ "$(ls -d /sys/block/nvme*n* 2>/dev/null | wc -l)" -lt 2 ] && [ $i -lt 30 ]; do
	sleep 1
	i=$((i + 1))
done
sleep 1
case "$(cat /proc/cmdline)" in
*nvme.write_window=*)
	run info
	run check
	run hash-outside --out $T/outside-before.txt
	run fence-test
	run soak --size-gib 1 --random 2000
	run verify --size-gib 1
	# an image the size of a small rootfs, streamed through zstd as from the Mac
	python3 -c 'import hashlib,sys; [sys.stdout.buffer.write(hashlib.shake_256(b"img%d" % i).digest(1 << 20)) for i in range(256)]' >/tmp/img
	sum=$(sha256sum /tmp/img | cut -d" " -f1)
	zstd -q -c /tmp/img | zstd -q -dc | python3 $T/nvme-install.py write-image --layout $L --bytes 268435456 --sha256 "$sum"
	echo "=== rc=$? (write-image)"
	run verify-image --bytes 268435456 --sha256 "$sum"
	run hash-outside --compare $T/outside-before.txt
	sed 's/^P2_FIRST=.*/P2_FIRST=3146496/' $L >/tmp/stale.env
	echo "=== soak with a layout whose p2 doesn't match the disk"
	python3 $T/nvme-install.py soak --layout /tmp/stale.env --size-gib 1
	echo "=== rc=$? (stale-layout)"
	;;
*)
	run check --no-window
	run soak --size-gib 1
	run hash-outside
	;;
esac
echo "TOOL-TEST done"
sync
poweroff -f
