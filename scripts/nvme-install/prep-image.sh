#!/usr/bin/env bash
# Make the image that goes onto the iPad's NVMe root partition (p2), on the
# Mac, from the Omarchy NBD image:
#   APFS clone of build/omarchy/rootfs.img -> e2fsck -> resize2fs to exactly
#   p2's size -> truncate -> label omarchy-nvme, UUID from layout.env ->
#   e2fsck -fn -> sha256.
# Output: build/nvme-install/rootfs-nvme.img (+ .sha256). The 24 GiB source
# is only cloned (cp -c: no space until blocks change; resize2fs moves some).
#
#   scripts/nvme-install/prep-image.sh [--fresh] [SOURCE]
#
# Only while nothing uses the source: no serve-root.sh/qemu-nbd serving it
# (the iPad would be running from it) and no Omarchy build container. The
# clone is then a cleanly unmounted filesystem.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="${NVI_BUILD:-$ROOT/build/nvme-install}"
FRESH=0
[ "${1:-}" = --fresh ] && { FRESH=1; shift; }
SRC="${1:-$ROOT/build/omarchy/rootfs.img}"
OUT="$B/rootfs-nvme.img"
[ -f "$B/layout.env" ] || { echo "no $B/layout.env (plan-layout.py)" >&2; exit 1; }
# shellcheck disable=SC1091
. "$B/layout.env"
[ -f "$SRC" ] || { echo "missing $SRC" >&2; exit 1; }
if pgrep -f "qemu-nbd.*$(basename "$SRC")" >/dev/null; then
	echo "qemu-nbd is serving $(basename "$SRC"): the iPad may be running from it. Shut the iPad down and stop serve-root.sh first." >&2
	exit 1
fi
if docker ps --format '{{.Image}}' 2>/dev/null | grep -q '^linux-a9-alarm'; then
	echo "an Omarchy build container is running (it may have the image mounted)" >&2
	exit 1
fi
if [ -e "$OUT" ] && [ "$FRESH" = 0 ]; then
	echo "$OUT exists (pass --fresh to rebuild it from $SRC)" >&2
	exit 1
fi
rm -f "$OUT" "$OUT.sha256"
cp -c "$SRC" "$OUT"
echo "cloned $SRC -> $OUT; resizing to p2: $P2_BYTES bytes ($((P2_BYTES / 4096)) blocks)"

docker run --rm --platform linux/arm64 -v "$B:/b" -e P2_BYTES="$P2_BYTES" -e FS_LABEL="$FS_LABEL" \
	-e FS_UUID="$FS_UUID" alpine:edge sh -euc '
apk add --no-cache e2fsprogs e2fsprogs-extra >/dev/null
img=/b/rootfs-nvme.img
bs=$(dumpe2fs -h $img 2>/dev/null | awk -F: "/^Block size/{gsub(/ /,\"\",\$2); print \$2}")
[ "$bs" = 4096 ] || { echo "filesystem block size $bs, expected 4096" >&2; exit 1; }
# -p fixes only what is safe without questions; 1 means "fixed something"
e2fsck -fp $img || [ $? -le 1 ] || { echo "e2fsck found problems it would not fix" >&2; exit 1; }
blocks=$((P2_BYTES / 4096))
resize2fs $img $blocks
truncate -s $P2_BYTES $img
tune2fs -L "$FS_LABEL" -U "$FS_UUID" $img >/dev/null
e2fsck -fn $img
got=$(dumpe2fs -h $img 2>/dev/null | awk -F: "/^Block count/{gsub(/ /,\"\",\$2); print \$2}")
[ "$got" = "$blocks" ] || { echo "block count $got, expected $blocks" >&2; exit 1; }
dumpe2fs -h $img 2>/dev/null | grep -E "^(Filesystem volume name|Filesystem UUID|Block count|Free blocks|Inode count|Free inodes)"'

[ "$(stat -f %z "$OUT")" = "$P2_BYTES" ] || { echo "image is $(stat -f %z "$OUT") bytes, p2 is $P2_BYTES" >&2; exit 1; }
shasum -a 256 "$OUT" | sed "s|  .*|  $(basename "$OUT")|" >"$OUT.sha256"
cat "$OUT.sha256"
echo "ready: $OUT ($P2_BYTES bytes, label $FS_LABEL, UUID $FS_UUID)"
