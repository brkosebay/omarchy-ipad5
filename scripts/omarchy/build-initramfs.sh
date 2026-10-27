#!/usr/bin/env bash
# Build build/omarchy/initramfs-nbd.gz: the NBD-root initramfs that boots the
# Omarchy rootfs served from the Mac (scripts/omarchy/serve-root.sh).
#
# Contents: scripts/omarchy/initramfs/init; HoolockRD's init_functions.sh
# (third_party/hoolock-rd, GPLv3, its USB gadget/DHCP/getty logic, unchanged);
# BusyBox, busybox-extras and unudhcpd from Alpine edge, as HoolockRD's own
# build takes them; a static nbd-client (scripts/omarchy/build-nbd-client.sh);
# and this unit's firmware from build/firmware (private: the image stays in
# build/), so drivers that load firmware at probe time find it before the
# root is mounted.
#
# m1n1 replaces the initrd with each gzip member of the payload, so the output
# is ONE gzip stream of ONE cpio archive (checked below).
#
# Usage: scripts/omarchy/build-initramfs.sh [--no-firmware]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/omarchy"
WORK="$OUT/initramfs"
FW="$ROOT/build/firmware/lib/firmware"
WITH_FW=1
[ "${1:-}" = "--no-firmware" ] && WITH_FW=0

[ -x "$WORK/nbd-client" ] || "$ROOT/scripts/omarchy/build-nbd-client.sh"
fw_mount=()
if [ "$WITH_FW" = 1 ] && [ -d "$FW" ]; then
	fw_mount=(-v "$FW:/fw:ro")
fi

docker run --rm --platform linux/arm64 \
	-v "$ROOT/scripts/omarchy/initramfs:/src:ro" \
	-v "$ROOT/third_party/hoolock-rd/scripts:/hoolock:ro" \
	-v "$WORK:/work" \
	-v "$OUT:/out" \
	"${fw_mount[@]}" \
	alpine:edge sh -euc '
apk add --no-cache busybox busybox-extras unudhcpd cpio gzip python3 >/dev/null
r=/tmp/rootfs
rm -rf "$r"
mkdir -p "$r"/bin "$r"/sbin "$r"/usr/bin "$r"/usr/sbin "$r"/lib "$r"/proc "$r"/sys \
	"$r"/dev "$r"/run "$r"/tmp "$r"/newroot "$r"/config "$r"/etc
install -m 0755 /src/init "$r/init"
install -m 0644 /hoolock/init_functions.sh "$r/init_functions.sh"
install -m 0755 /bin/busybox "$r/bin/busybox"
install -m 0755 "$(command -v busybox-extras)" "$r/bin/busybox-extras"
install -m 0755 "$(command -v unudhcpd)" "$r/usr/bin/unudhcpd"
install -m 0755 /work/nbd-client "$r/sbin/nbd-client"
install -m 0755 /lib/ld-musl-aarch64.so.1 "$r/lib/ld-musl-aarch64.so.1"
ln -s ld-musl-aarch64.so.1 "$r/lib/libc.musl-aarch64.so.1"
mknod -m 0600 "$r/dev/console" c 5 1
mknod -m 0666 "$r/dev/null" c 1 3
if [ -d /fw ]; then
	mkdir -p "$r/lib/firmware"
	cp -r /fw/. "$r/lib/firmware/"
fi
# Every binary must find its libraries inside the initramfs.
for b in "$r/bin/busybox" "$r/bin/busybox-extras" "$r/usr/bin/unudhcpd"; do
	for lib in $(ldd "$b" 2>/dev/null | awk "/=>/{print \$3}"); do
		[ -e "$r$lib" ] || [ -e "$r/lib/${lib##*/}" ] || { echo "missing $lib for $b" >&2; exit 1; }
	done
done
"$r/bin/busybox" sh -n "$r/init"
# BusyBox installs its own nbd-client applet ahead of /sbin in PATH.
if grep -nE "^[[:space:]]*(if !? *)?nbd-client " "$r/init"; then
	echo "init runs nbd-client by name; use \$NBD_CLIENT (/sbin/nbd-client)" >&2
	exit 1
fi
for applet in switch_root getty md5sum ip mountpoint killall telnetd udhcpd setsid; do
	"$r/bin/busybox" --list | grep -qx "$applet" ||
		"$r/bin/busybox-extras" --list | grep -qx "$applet" || { echo "busybox lacks $applet" >&2; exit 1; }
done
cd "$r"
find . -mindepth 1 | sort | cpio -o -H newc -R 0:0 --quiet | gzip -9 -n >/out/initramfs-nbd.gz
python3 - /out/initramfs-nbd.gz <<EOF
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
print("single gzip stream")
EOF
echo "busybox: $("$r/bin/busybox" | head -1)"
'
echo "built: $OUT/initramfs-nbd.gz ($(du -h "$OUT/initramfs-nbd.gz" | cut -f1))"
echo "payload: TREE=omarchy scripts/mkpayload.sh $OUT/initramfs-nbd.gz"
