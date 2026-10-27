#!/bin/busybox sh
# /init of the QEMU write-window test (scripts/nvme-install/qemu-test.sh
# fence). The same probes run on every boot; the host knows what each boot's
# command line should allow and checks the RESULT lines and the disk images.
/bin/busybox --install -s /bin
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev
echo "FENCE-TEST start: $(cat /proc/cmdline)"
i=0
while [ "$(ls -d /sys/block/nvme*n* 2>/dev/null | wc -l)" -lt 2 ] && [ $i -lt 30 ]; do
	sleep 1
	i=$((i + 1))
done
# nvmeXnY numbering follows the scan order, not the NSID (seen here: NSID 2
# came up as nvme0n1 on some boots), so find each namespace by its NSID.
for b in /sys/block/nvme*n*; do
	d=${b##*/}
	n=$(cat "$b/nsid")
	eval "NS$n=/dev/$d"
	q=$b/queue
	echo "SYSFS ns$n dev=$d ro=$(cat $b/ro) lbs=$(cat $q/logical_block_size) discard=$(cat $q/discard_max_bytes) wz=$(cat $q/write_zeroes_max_bytes) wc=$(cat $q/write_cache | tr ' ' _) fua=$(cat $q/fua)"
done
echo "PARAM write_window=$(cat /sys/module/nvme/parameters/write_window 2>&1)"

t() {
	WTEST_TAG=$1
	export WTEST_TAG
	shift
	/bin/wtest "$@"
}
# ns1 is 16384 LBAs; the window in boot A is 4096-12287.
t in_first write $NS1 4096 1 0xa1
t in_last write $NS1 12287 1 0xa2
t in_big write $NS1 6000 1024 0xa3
t below write $NS1 4095 1 0xb1
t above write $NS1 12288 1 0xb2
t straddle_lo write $NS1 4095 2 0xb3
t straddle_hi write $NS1 12287 2 0xb4
t lba0 write $NS1 0 1 0xb5
t end write $NS1 16383 1 0xb6
t ns2_openrw openrw $NS2
t ns2_write write $NS2 0 1 0xc1
t discard_in discard $NS1 8192 16
t zeroout_in zeroout $NS1 8192 16
t zeroout_out zeroout $NS1 100 16
dmesg | grep -iE 'nvme|write_window' | sed 's/^/DMESG /'
echo "FENCE-TEST done"
sync
poweroff -f
