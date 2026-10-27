#!/usr/bin/env bash
# Offline test of the GPT path, before anything runs on the iPad:
#   plan-layout.py (plans from dumps) -> gptcas (writes, Linux build) -> sgdisk
#   checks the result -> gptcas --reverse restores the original byte for byte,
# on a 4 KiB-sector loop device of exactly the iPad's size (32,000,000,000
# bytes) holding an iOS-shaped GPT (one APFS partition, LBAs 6..N-6) and
# synthetic APFS checkpoints for gptcas's container-size guard. Then the
# macOS build of gptcas on a sparse file of the same geometry.
# Output: build/nvme-install/test-gpt/report.txt
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="$ROOT/build/nvme-install/test-gpt"
rm -rf "$W"
mkdir -p "$W"

docker run --rm --privileged --platform linux/arm64 -v "$ROOT/scripts/nvme-install:/s:ro" -v "$W:/w" \
	alpine:edge sh -euc '
apk add --no-cache sgdisk util-linux python3 gcc musl-dev linux-headers >/dev/null
gcc -O2 -Wall -o /tmp/gptcas /s/ios/gptcas.c
d=/tmp/disk.img
truncate -s 32000000000 $d
l=$(losetup -P -b 4096 -f --show $d)
trap "losetup -d $l" EXIT
# iOS-shaped: 2-LBA alignment keeps p1 at LBA 6, as on the iPad (Hoolock gdisk tutorial)
sgdisk -a 2 -n 1:6:0 -t 1:AF0A -c 1:Container $l >/dev/null
sgdisk -p $l | tail -2

nxsb() { # nxsb LBA_OF_P1 BLOCK XID BLOCK_COUNT: one valid container superblock
	python3 -I - "$l" "$@" <<EOF
import os, struct, sys
dev, p1, blk, xid, count = sys.argv[1], *map(int, sys.argv[2:])
def f64(b):
    s1 = s2 = 0
    for w in struct.unpack_from("<1022I", b, 8):
        s1 = (s1 + w) % 0xFFFFFFFF; s2 = (s2 + s1) % 0xFFFFFFFF
    c1 = 0xFFFFFFFF - ((s1 + s2) % 0xFFFFFFFF); c2 = 0xFFFFFFFF - ((s1 + c1) % 0xFFFFFFFF)
    return (c2 << 32) | c1
b = bytearray(4096)
struct.pack_into("<QQII", b, 8, 1, xid, 0x80000001, 0)
struct.pack_into("<IIQ", b, 32, 0x4253584E, 4096, count)
struct.pack_into("<I", b, 0x68, 8); struct.pack_into("<Q", b, 0x70, 1)
struct.pack_into("<Q", b, 0, f64(b))
fd = os.open(dev, os.O_WRONLY); os.pwrite(fd, b, (p1 + blk) * 4096); os.fsync(fd); os.close(fd)
EOF
}
N=7812500
FULL=7812489
# block 0 + checkpoint area: newest (xid 7) still the full size, an older one (xid 6) small
nxsb 6 0 7 $FULL; nxsb 6 1 7 $FULL; nxsb 6 2 6 3145728

mkdir -p /w/backup
dd if=$l of=/w/backup/nvme0n1-first-1MiB.bin bs=4096 count=256 status=none
dd if=$l of=/w/backup/nvme0n1-last-1MiB.bin bs=4096 skip=$((N - 256)) count=256 status=none
printf "DISK_BYTES=%s\nLBA_SIZE=%s\n" "$(blockdev --getsize64 $l)" "$(blockdev --getss $l)" >/w/backup/geometry.env
printf "APFS_BLOCK_SIZE=4096\nAPFS_BLOCK_COUNT=%s\nAPFS_USED_BYTES=%s\n" $FULL $((9 << 30)) >/w/backup/apfs.env
python3 -I /s/apfs-nx.py $l --offset $((6 * 4096)) 2>&1 | sed "s/^/apfs-nx: /" || true

echo "## plan"
python3 -I /s/plan-layout.py --backup /w/backup --apfs /w/backup/apfs.env --out /w
. /w/layout.env
P=/w/gpt-plan
ok() { echo "PASS $*"; }
bad() { echo "FAIL $*"; }
expect_rc() { want=$1; shift; set +e; "$@" >/tmp/out 2>&1; rc=$?; set -e; sed "s/^/    /" /tmp/out; [ $rc = $want ]; }

echo "## gptcas while the container is still full size (newest checkpoint)"
expect_rc 3 /tmp/gptcas $l $P && ok "refused: container does not fit yet" || bad "not refused"
cmp -s /w/backup/nvme0n1-first-1MiB.bin <(dd if=$l bs=4096 count=256 status=none) && ok "nothing written" || bad "disk changed"

echo "## after a (simulated) resize: newest checkpoint xid 9 says $APFS_BYTES bytes"
nxsb 6 3 9 $((APFS_BYTES / 4096))
expect_rc 0 /tmp/gptcas --dry-run $l $P && ok "dry run passes" || bad "dry run"
# interrupted between the copies: only the new primary is on the disk
dd if=$P/new-primary.bin of=$l bs=4096 seek=1 conv=notrunc,fsync status=none
expect_rc 0 /tmp/gptcas $l $P && grep -q "primary GPT .*already the new one" /tmp/out &&
	grep -q "backup GPT .*the original, to be replaced" /tmp/out && ok "half-written GPT finished by a re-run" || bad "half-written apply"
expect_rc 0 /tmp/gptcas $l $P && grep -q "already holds" /tmp/out && ok "second run is a no-op" || bad "second run"

echo "## sgdisk on the result"
sgdisk -v $l | grep -q "No problems found" && ok "sgdisk -v: no problems" || { sgdisk -v $l; bad "sgdisk -v"; }
sgdisk -p $l | tail -3
i1=$(sgdisk -i 1 $l); i2=$(sgdisk -i 2 $l)
echo "$i1" | grep -qi "unique GUID: $P1_PARTUUID" && ok "p1 unique GUID preserved" || bad "p1 GUID"
echo "$i1" | grep -q "First sector: 6 " && echo "$i1" | grep -q "Last sector: $P1_LAST " && ok "p1 6-$P1_LAST" || bad "p1 range"
echo "$i1" | grep -q "Partition name: .Container." && ok "p1 name preserved" || bad "p1 name"
echo "$i2" | grep -qi "GUID code: 0FC63DAF-8483-4772-8E79-3D69D8477DE4" && ok "p2 type Linux filesystem" || bad "p2 type"
echo "$i2" | grep -qi "unique GUID: $P2_PARTUUID" && ok "p2 unique GUID from layout.env" || bad "p2 GUID"
echo "$i2" | grep -q "First sector: $P2_FIRST " && echo "$i2" | grep -q "Last sector: $P2_LAST " && ok "p2 $P2_FIRST-$P2_LAST" || bad "p2 range"
blockdev --rereadpt $l
p2=/sys/class/block/${l##*/}p2
[ "$(cat $p2/start)" = $((P2_FIRST * 8)) ] && [ "$(cat $p2/size)" = $((P2_LBAS * 8)) ] &&
	ok "kernel sees p2 as exactly the window $WRITE_WINDOW" || bad "kernel p2 $(cat $p2/start)+$(cat $p2/size)"
[ $((P2_FIRST % 256)) = 0 ] && [ $(((P2_LAST + 1) % 256)) = 0 ] && ok "p2 is 1 MiB aligned" || bad "alignment"
[ $((GAPA_LAST - GAPA_FIRST + 1)) -ge 256 ] && [ $((GAPB_LAST - GAPB_FIRST + 1)) -ge 256 ] && ok "gaps >= 1 MiB on both sides" || bad "gaps"

echo "## replanning"
python3 -I /s/plan-layout.py --backup /w/backup --apfs /w/backup/apfs.env --out /w >/tmp/out && ok "replan from the same dumps reproduces the plan" || bad "replan"
mkdir -p /w/after; cp /w/backup/geometry.env /w/backup/apfs.env /w/after/
dd if=$l of=/w/after/nvme0n1-first-1MiB.bin bs=4096 count=256 status=none
dd if=$l of=/w/after/nvme0n1-last-1MiB.bin bs=4096 skip=$((N - 256)) count=256 status=none
python3 -I /s/plan-layout.py --backup /w/after --apfs /w/after/apfs.env --out /w | grep -q "already has the planned p2" && ok "planner recognises the applied GPT" || bad "planner on applied GPT"

echo "## tampering and rollback"
python3 -I -c "import os,sys; fd=os.open(sys.argv[1],os.O_RDWR); o=($N-1)*4096+200; b=os.pread(fd,1,o); os.pwrite(fd,bytes([b[0]^1]),o); os.fsync(fd)" $l
expect_rc 3 /tmp/gptcas --reverse $l $P && ok "reverse refused on a modified backup GPT" || bad "tamper not caught"
python3 -I -c "import os,sys; fd=os.open(sys.argv[1],os.O_RDWR); o=($N-1)*4096+200; b=os.pread(fd,1,o); os.pwrite(fd,bytes([b[0]^1]),o); os.fsync(fd)" $l
# interrupted reverse: only the original backup is back
dd if=$P/old-backup.bin of=$l bs=4096 seek=$((N - 5)) conv=notrunc,fsync status=none
expect_rc 0 /tmp/gptcas --reverse $l $P && grep -q "backup GPT .*already the original" /tmp/out &&
	ok "half-done reverse finished" || bad "reverse"
cmp -s <(dd if=$l bs=4096 count=6 status=none) <(head -c 24576 /w/backup/nvme0n1-first-1MiB.bin) &&
cmp -s <(dd if=$l bs=4096 skip=$((N - 5)) count=5 status=none) <(tail -c 20480 /w/backup/nvme0n1-last-1MiB.bin) &&
	ok "original GPT restored byte for byte" || bad "restore differs"
sgdisk -v $l | grep -q "No problems found" && ok "sgdisk -v after reverse" || bad "sgdisk -v after reverse"
' | tee "$W/report.txt"

echo "## macOS build of gptcas on a sparse file" | tee -a "$W/report.txt"
F="$W/mac-disk.img"
rm -f "$F"
truncate -s 32000000000 "$F"
python3 -I - "$F" "$W" <<'EOF' >>"$W/report.txt"
import os, struct, sys
f, w = sys.argv[1], sys.argv[2]
fd = os.open(f, os.O_RDWR)
first = open(w + "/backup/nvme0n1-first-1MiB.bin", "rb").read()
last = open(w + "/backup/nvme0n1-last-1MiB.bin", "rb").read()
os.pwrite(fd, first, 0)
os.pwrite(fd, last, 32000000000 - len(last))
env = dict(l.strip().split("=", 1) for l in open(w + "/layout.env") if "=" in l and not l.startswith("#"))
# newest checkpoint says the container already fits the new p1
def f64(b):
    s1 = s2 = 0
    for x in struct.unpack_from("<1022I", b, 8):
        s1 = (s1 + x) % 0xFFFFFFFF; s2 = (s2 + s1) % 0xFFFFFFFF
    c1 = 0xFFFFFFFF - ((s1 + s2) % 0xFFFFFFFF); c2 = 0xFFFFFFFF - ((s1 + c1) % 0xFFFFFFFF)
    return (c2 << 32) | c1
b = bytearray(4096)
struct.pack_into("<QQII", b, 8, 1, 11, 0x80000001, 0)
struct.pack_into("<IIQ", b, 32, 0x4253584E, 4096, int(env["APFS_BYTES"]) // 4096)
struct.pack_into("<I", b, 0x68, 8); struct.pack_into("<Q", b, 0x70, 1)
struct.pack_into("<Q", b, 0, f64(b))
os.pwrite(fd, b, (6 + 4) * 4096)
os.close(fd)
print("    sparse file prepared")
EOF
G="$ROOT/build/nvme-install/mac/gptcas"
[ -x "$G" ] || "$ROOT/scripts/nvme-install/build-ios-tools.sh" >/dev/null
{
	"$G" --dry-run "$F" "$W/gpt-plan" && echo "PASS macOS dry run" || echo "FAIL macOS dry run"
	"$G" "$F" "$W/gpt-plan" && echo "PASS macOS apply" || echo "FAIL macOS apply"
	cmp -s <(dd if="$F" bs=4096 skip=1 count=5 2>/dev/null) "$W/gpt-plan/new-primary.bin" &&
		cmp -s <(dd if="$F" bs=4096 skip=7812495 count=5 2>/dev/null) "$W/gpt-plan/new-backup.bin" &&
		echo "PASS macOS: the file holds the planned GPT" || echo "FAIL macOS content"
	"$G" --reverse "$F" "$W/gpt-plan" && echo "PASS macOS reverse" || echo "FAIL macOS reverse"
} 2>&1 | tee -a "$W/report.txt"
rm -f "$F"
echo "test-gpt: $(grep -c '^PASS' "$W/report.txt") PASS, $(grep -c '^FAIL' "$W/report.txt") FAIL"
! grep -q '^FAIL' "$W/report.txt"
