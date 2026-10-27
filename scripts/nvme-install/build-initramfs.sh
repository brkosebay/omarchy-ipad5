#!/usr/bin/env bash
# build/nvme-install/initramfs-omarchy.gz: the Omarchy initramfs
# (build/omarchy/initramfs-nbd.gz, from scripts/omarchy/build-initramfs.sh)
# with /init replaced by the current scripts/omarchy/initramfs/init, which
# adds omarchy.root=nvme. Everything else (BusyBox, nbd-client, firmware,
# HoolockRD's init_functions.sh) is the base's, byte for byte. Leaves
# build/omarchy/ alone; scripts/omarchy/build-initramfs.sh gives the same
# init once the Omarchy workstream rebuilds.
#
# Checks: the init parses with the base's BusyBox, every applet it needs is
# there, and the result is one gzip stream (m1n1 keeps only the last member).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE="$ROOT/build/omarchy/initramfs-nbd.gz"
OUT="$ROOT/build/nvme-install"
[ -f "$BASE" ] || { echo "missing $BASE (scripts/omarchy/build-initramfs.sh)" >&2; exit 1; }
mkdir -p "$OUT"

docker run --rm --platform linux/arm64 \
	-v "$ROOT/scripts/omarchy/initramfs:/src:ro" -v "$BASE:/base.gz:ro" -v "$OUT:/out" \
	alpine:edge sh -euc '
apk add --no-cache cpio gzip >/dev/null
b=/tmp/base; o=/tmp/overlay
mkdir -p $b $o
cd $b && gzip -dc /base.gz | cpio -id --quiet
"$b/lib/ld-musl-aarch64.so.1" "$b/bin/busybox" sh -n /src/init
for a in dd od tr ping awk cat sleep mount mountpoint switch_root; do
	"$b/lib/ld-musl-aarch64.so.1" "$b/bin/busybox" --list | grep -qx "$a" || { echo "base BusyBox lacks $a" >&2; exit 1; }
done
install -m 0755 /src/init $o/init
cd $o && echo init | cpio -o -H newc -R 0:0 --quiet | gzip -9n >/out/.init-overlay.gz'
python3 -I "$ROOT/scripts/merge-initramfs.py" "$OUT/initramfs-omarchy.gz" "$BASE" "$OUT/.init-overlay.gz"
rm -f "$OUT/.init-overlay.gz"
python3 -I - "$OUT/initramfs-omarchy.gz" <<'EOF'
import sys, zlib
data = open(sys.argv[1], "rb").read()
members = 0
while data:
    d = zlib.decompressobj(31)
    d.decompress(data)
    if not d.eof:
        sys.exit("truncated gzip stream")
    members += 1
    data = d.unused_data
if members != 1:
    sys.exit(f"{members} gzip members; m1n1 would keep only the last")
EOF
echo "built: $OUT/initramfs-omarchy.gz ($(du -h "$OUT/initramfs-omarchy.gz" | cut -f1)), init $(shasum -a 256 "$ROOT/scripts/omarchy/initramfs/init" | cut -c1-12)"
