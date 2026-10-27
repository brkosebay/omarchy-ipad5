#!/usr/bin/env bash
# Read-only backup of what the NVMe install will change, from Linux on the
# iPad (any kernel; the read-only one is fine), to the Mac, BEFORE the first
# write (which is iOS's resize_apfs). Run on the Mac:
#
#   scripts/nvme-install/backup.sh [--full] [OUTDIR]   # default build/nvme-install/backup
#
# Writes to OUTDIR (refuses if it exists: the first backup is the one that
# matters):
#   nvme0n1-first-1MiB.bin   NSID 1, LBAs 0-255 (PMBR, primary GPT, start of APFS)
#   nvme0n1-last-1MiB.bin    NSID 1, last 256 LBAs (end of APFS, backup GPT)
#   geometry.env             DISK_BYTES, LBA_SIZE, the NSID 1 device, p1 start/size
#   apfs.env                 apfs-nx.py on p1: container size and used space
#   sfdisk.txt, info.txt     partition table as text; NSID map, cache/trim flags
#   SHA256SUMS               of the dumps, checked against hashes taken on the device
#   nvme0n1-full.img.zst     only with --full: all of NSID 1 (~15-25 min over USB),
#                            the way to put this exact iOS back without a restore
# NSIDs 2-7 were dumped and verified earlier (build/firmware/private/nvme-ns/).
#
# The iPad must run Linux with SSH up (scripts/omarchy/ssh.sh). Nothing on the
# iPad is written; every device command is a read.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FULL=0
[ "${1:-}" = --full ] && { FULL=1; shift; }
OUT="${1:-$ROOT/build/nvme-install/backup}"
SSH="$ROOT/scripts/omarchy/ssh.sh"
[ -e "$OUT" ] && { echo "$OUT exists; the first backup must not be overwritten (pass another OUTDIR)" >&2; exit 1; }
mkdir -p "$OUT"

# NSID 1's device, by its NSID (nvmeXnY numbering follows the scan order)
# (; true: the loop's status is its last test's, which set -e would trip on)
D=$("$SSH" 'for b in /sys/block/nvme*n*; do [ "$(cat $b/nsid)" = 1 ] && echo ${b##*/}; done; true')
[ -n "$D" ] || { echo "no NVMe namespace with NSID 1 on the iPad" >&2; exit 1; }
read -r BYTES LBS P1S P1N < <("$SSH" "echo \$(sudo blockdev --getsize64 /dev/$D) \$(cat /sys/block/$D/queue/logical_block_size) \$(cat /sys/block/$D/${D}p1/start) \$(cat /sys/block/$D/${D}p1/size)")
[ "$LBS" = 4096 ] || { echo "LBA size $LBS, expected 4096" >&2; exit 1; }
N=$((BYTES / LBS))
cat >"$OUT/geometry.env" <<EOF
# scripts/nvme-install/backup.sh, $(date -u +%FT%TZ)
NSID1_DEV=$D
DISK_BYTES=$BYTES
LBA_SIZE=$LBS
DISK_LBAS=$N
P1_START_512=$P1S
P1_SIZE_512=$P1N
EOF
echo "NSID 1 is $D: $BYTES bytes, $N LBAs of $LBS; p1 at 512-byte sector $P1S, $P1N sectors"

"$SSH" "sudo dd if=/dev/$D bs=4096 count=256 iflag=direct status=none" >"$OUT/nvme0n1-first-1MiB.bin"
"$SSH" "sudo dd if=/dev/$D bs=4096 skip=$((N - 256)) count=256 iflag=direct status=none" >"$OUT/nvme0n1-last-1MiB.bin"
dev_sums=$("$SSH" "sudo dd if=/dev/$D bs=4096 count=256 iflag=direct status=none | sha256sum; sudo dd if=/dev/$D bs=4096 skip=$((N - 256)) count=256 iflag=direct status=none | sha256sum" | cut -d' ' -f1)
(cd "$OUT" && shasum -a 256 nvme0n1-first-1MiB.bin nvme0n1-last-1MiB.bin >SHA256SUMS)
mac_sums=$(cut -d' ' -f1 "$OUT/SHA256SUMS")
if [ "$dev_sums" != "$mac_sums" ]; then
	echo "MISMATCH between the device's and the Mac's hashes of the dumps:" >&2
	echo "device: $dev_sums" >&2
	echo "mac:    $mac_sums" >&2
	exit 1
fi
echo "dumps verified (device sha256 = Mac sha256)"

# Not fatal: the dumps matter most. Without it, plan-layout.py takes
# --apfs-used-bytes from iOS's df (test-plan.md step 2).
if "$SSH" "sudo python3 - /dev/${D}p1" <"$ROOT/scripts/nvme-install/apfs-nx.py" >"$OUT/apfs.env"; then
	cat "$OUT/apfs.env"
else
	echo "WARNING: apfs-nx.py could not read the container (see above); plan with --apfs-used-bytes" >&2
fi
"$SSH" "sudo sfdisk -d /dev/$D" >"$OUT/sfdisk.txt"
"$SSH" 'for b in /sys/block/nvme*n*; do q=$b/queue; echo "NSID $(cat $b/nsid) ${b##*/} bytes=$(($(cat $b/size) * 512)) ro=$(cat $b/ro) lbs=$(cat $q/logical_block_size) write_cache=$(cat $q/write_cache | tr " " _) fua=$(cat $q/fua) discard_max=$(cat $q/discard_max_bytes) write_zeroes_max=$(cat $q/write_zeroes_max_bytes) max_hw_kb=$(cat $q/max_hw_sectors_kb)"; done
	echo "write_window=$(cat /sys/module/nvme/parameters/write_window 2>/dev/null || echo "(not in this kernel)")"
	uname -r; sudo dmesg | grep -E "read-only|write window|Apple ANS: namespaces|refused"; true' >"$OUT/info.txt"
cat "$OUT/info.txt"

if [ "$FULL" = 1 ]; then
	echo "full image of NSID 1 ($BYTES bytes) -> $OUT/nvme0n1-full.img.zst"
	"$SSH" "sudo dd if=/dev/$D bs=4M iflag=direct status=none | zstd -T2 -3 -c" >"$OUT/nvme0n1-full.img.zst"
	dev_full=$("$SSH" "sudo dd if=/dev/$D bs=4M iflag=direct status=none | sha256sum" | cut -d' ' -f1)
	mac_full=$(zstd -dc "$OUT/nvme0n1-full.img.zst" | shasum -a 256 | cut -d' ' -f1)
	[ "$dev_full" = "$mac_full" ] || { echo "full image MISMATCH: device $dev_full, Mac $mac_full" >&2; exit 1; }
	echo "$mac_full  nvme0n1-full.img" >>"$OUT/SHA256SUMS"
	echo "full image verified ($(du -h "$OUT/nvme0n1-full.img.zst" | cut -f1) compressed)"
fi
chmod a-w "$OUT"/*
echo "backup complete: $OUT"
